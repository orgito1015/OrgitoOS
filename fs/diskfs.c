/*
 * OrgitoOS - OrgitoFS: a small custom on-disk filesystem
 *
 * Layout on the ATA disk (drivers/ata.c), all offsets in 512-byte
 * sectors:
 *
 *   LBA 0                       superblock (1 sector)
 *   LBA 1 .. +inode_table_sectors   flat inode table (fixed-size array)
 *   LBA .. +bitmap_sectors          block bitmap (1 bit per data block)
 *   LBA data_start_lba ..           data blocks (1 sector = 1 block)
 *
 * Deliberately simple: a flat, fixed-size inode table (no free-inode
 * search structure beyond a linear scan - fine at DISKFS_MAX_INODES
 * scale) and no separate on-disk directory-entry blocks. Each inode
 * just stores its parent's inode index; the in-memory vfs_node_t tree
 * (parent/children pointers) is rebuilt from that at mount time via the
 * *same* vfs_add_child()/vfs_find_node()/vfs_remove_child() that
 * fs/vfs.c's ramfs already uses - those only touch vfs_node_t's shared
 * parent/children/child_count fields, so they work identically whether
 * a node is ram- or disk-backed. That reuse is also why directories
 * need no on-disk entry blocks at all: once children[]/child_count are
 * rebuilt, vfs_find_node()/vfs_resolve_path()/ls all just work.
 *
 * Files are capped at DISKFS_DIRECT_BLOCKS direct blocks - no indirect
 * blocks - sized to exactly match VFS_MAX_FILE_SIZE (see the static
 * assert below) so a disk-backed file can never overflow the
 * fixed-size buffers shell commands like `cat`/`write` already share
 * with ramfs files.
 */

#include "diskfs.h"
#include "vfs.h"
#include "../drivers/ata.h"
#include "../drivers/console.h"
#include "../kernel/string.h"

#define DISKFS_MAGIC          0x4F524749u   /* "ORGI" */
#define DISKFS_MAX_INODES     64
#define DISKFS_MAX_NAME       32
#define DISKFS_DIRECT_BLOCKS  8             /* 8 * 512 = VFS_MAX_FILE_SIZE */
#define DISKFS_NO_INODE       0xFFFFFFFFu
#define DISKFS_NO_BLOCK       0xFFFFFFFFu
#define DISKFS_TYPE_FILE      0
#define DISKFS_TYPE_DIR       1
#define DISKFS_ROOT_INODE     0u

#define DISKFS_MAX_FILE_SIZE (DISKFS_DIRECT_BLOCKS * ATA_SECTOR_SIZE)
typedef char diskfs_file_size_matches_vfs_max[(DISKFS_MAX_FILE_SIZE == VFS_MAX_FILE_SIZE) ? 1 : -1];

/* Generous fixed bound on the in-memory (and on-disk) block bitmap -
 * caps how much of a disk this driver can address: 4096 bytes * 8 bits
 * * 512 bytes/block = 16 MiB of data blocks. Plenty for a hobby-OS demo
 * disk image without needing a dynamically sized bitmap. */
#define DISKFS_MAX_BITMAP_BYTES 4096u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t inode_table_start_lba;
    uint32_t inode_table_sectors;
    uint32_t bitmap_start_lba;
    uint32_t bitmap_sectors;
    uint32_t data_start_lba;
} diskfs_superblock_t;

typedef struct __attribute__((packed)) {
    uint8_t  used;
    uint8_t  type;
    char     name[DISKFS_MAX_NAME];
    uint32_t size;
    uint32_t parent;
    uint32_t blocks[DISKFS_DIRECT_BLOCKS];
} diskfs_inode_t;

static diskfs_superblock_t sb;
static diskfs_inode_t inode_table[DISKFS_MAX_INODES];
static uint8_t block_bitmap[DISKFS_MAX_BITMAP_BYTES];

/* disk_nodes[i] is the in-memory vfs_node_t for inode_table[i] - the
 * two arrays are always index-parallel, so a node's `inode` field is
 * simultaneously its slot in both. */
static vfs_node_t disk_nodes[DISKFS_MAX_INODES];
static vfs_dirent_t disk_dirent;
static int disk_mounted = 0;

/* ------------------------------------------------------------------ */
/* Block bitmap                                                        */
/* ------------------------------------------------------------------ */

static int bitmap_test(uint32_t bit) {
    return (block_bitmap[bit / 8] >> (bit % 8)) & 1;
}
static void bitmap_set(uint32_t bit) {
    block_bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}
static void bitmap_clear(uint32_t bit) {
    block_bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}
static uint32_t bitmap_alloc(void) {
    for (uint32_t i = 0; i < sb.total_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            return i;
        }
    }
    return DISKFS_NO_BLOCK;
}

/* ------------------------------------------------------------------ */
/* Metadata persistence                                                 */
/* ------------------------------------------------------------------ */

static void diskfs_flush_superblock(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    memset(sector, 0, sizeof(sector));
    memcpy(sector, &sb, sizeof(sb));
    ata_write_sectors(0, 1, sector);
}

static void diskfs_flush_bitmap(void) {
    for (uint32_t s = 0; s < sb.bitmap_sectors; s++) {
        ata_write_sectors(sb.bitmap_start_lba + s, 1, block_bitmap + (size_t)s * ATA_SECTOR_SIZE);
    }
}

static void diskfs_flush_inode_table(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    const uint8_t *src = (const uint8_t *)inode_table;
    uint32_t total_bytes = (uint32_t)sizeof(inode_table);
    for (uint32_t s = 0; s < sb.inode_table_sectors; s++) {
        uint32_t off = s * ATA_SECTOR_SIZE;
        uint32_t chunk = (total_bytes > off) ? (total_bytes - off) : 0;
        if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
        memset(sector, 0, sizeof(sector));
        if (chunk > 0) memcpy(sector, src + off, chunk);
        ata_write_sectors(sb.inode_table_start_lba + s, 1, sector);
    }
}

static int diskfs_load_superblock(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    if (ata_read_sectors(0, 1, sector) != 0) {
        return -1;
    }
    memcpy(&sb, sector, sizeof(sb));
    return (sb.magic == DISKFS_MAGIC) ? 0 : -1;
}

static void diskfs_load_bitmap_and_inodes(void) {
    for (uint32_t s = 0; s < sb.bitmap_sectors && (size_t)s * ATA_SECTOR_SIZE < DISKFS_MAX_BITMAP_BYTES; s++) {
        ata_read_sectors(sb.bitmap_start_lba + s, 1, block_bitmap + (size_t)s * ATA_SECTOR_SIZE);
    }

    uint8_t sector[ATA_SECTOR_SIZE];
    uint8_t *dst = (uint8_t *)inode_table;
    uint32_t total_bytes = (uint32_t)sizeof(inode_table);
    for (uint32_t s = 0; s < sb.inode_table_sectors; s++) {
        ata_read_sectors(sb.inode_table_start_lba + s, 1, sector);
        uint32_t off = s * ATA_SECTOR_SIZE;
        uint32_t chunk = (total_bytes > off) ? (total_bytes - off) : 0;
        if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
        if (chunk > 0) memcpy(dst + off, sector, chunk);
    }
}

/* Formats a brand-new, empty OrgitoFS: lays out the superblock/inode
 * table/bitmap regions based on the attached disk's actual size, then
 * writes a single root directory inode (index 0, named "mnt" - the
 * name it will be mounted under). */
static void diskfs_format(void) {
    memset(&sb, 0, sizeof(sb));
    sb.magic = DISKFS_MAGIC;
    sb.inode_count = DISKFS_MAX_INODES;

    uint32_t inode_table_bytes = (uint32_t)sizeof(inode_table);
    uint32_t inode_table_sectors = (inode_table_bytes + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    uint32_t bitmap_sectors = DISKFS_MAX_BITMAP_BYTES / ATA_SECTOR_SIZE;

    sb.inode_table_start_lba = 1;
    sb.inode_table_sectors   = inode_table_sectors;
    sb.bitmap_start_lba      = sb.inode_table_start_lba + inode_table_sectors;
    sb.bitmap_sectors        = bitmap_sectors;
    sb.data_start_lba        = sb.bitmap_start_lba + bitmap_sectors;

    uint32_t disk_sectors = ata_sector_count();
    uint32_t reserved = sb.data_start_lba;
    uint32_t available = (disk_sectors > reserved) ? (disk_sectors - reserved) : 0;
    uint32_t max_by_bitmap = DISKFS_MAX_BITMAP_BYTES * 8u;
    sb.total_blocks = (available < max_by_bitmap) ? available : max_by_bitmap;

    memset(block_bitmap, 0, sizeof(block_bitmap));
    memset(inode_table, 0, sizeof(inode_table));

    diskfs_inode_t *root = &inode_table[DISKFS_ROOT_INODE];
    root->used = 1;
    root->type = DISKFS_TYPE_DIR;
    strncpy(root->name, "mnt", DISKFS_MAX_NAME - 1);
    root->parent = DISKFS_ROOT_INODE;   /* self-parented, like vfs.c's ramfs root */
    for (int b = 0; b < DISKFS_DIRECT_BLOCKS; b++) root->blocks[b] = DISKFS_NO_BLOCK;

    diskfs_flush_superblock();
    diskfs_flush_bitmap();
    diskfs_flush_inode_table();

    console_write("DISKFS: formatted a fresh OrgitoFS (");
    { char buf[12]; itoa((int)sb.total_blocks, buf, 10); console_write(buf); }
    console_write(" blocks, ");
    { char buf[12]; itoa((int)(sb.total_blocks / 2048), buf, 10); console_write(buf); }
    console_write(" MiB usable)\n");
}

/* ------------------------------------------------------------------ */
/* vfs_node_t operations                                                */
/* ------------------------------------------------------------------ */

static ssize_t diskfs_op_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || node->type != NODE_FILE) return -1;
    diskfs_inode_t *ino = &inode_table[node->inode];

    if (offset >= ino->size) return 0;
    uint32_t to_read = size;
    if (offset + to_read > ino->size) to_read = ino->size - offset;

    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < to_read) {
        uint32_t file_pos  = offset + done;
        uint32_t block_idx = file_pos / ATA_SECTOR_SIZE;
        uint32_t block_off = file_pos % ATA_SECTOR_SIZE;
        if (block_idx >= DISKFS_DIRECT_BLOCKS || ino->blocks[block_idx] == DISKFS_NO_BLOCK) {
            break;   /* sparse hole (shouldn't happen - size never exceeds allocated blocks) */
        }
        if (ata_read_sectors(sb.data_start_lba + ino->blocks[block_idx], 1, sector) != 0) {
            break;
        }
        uint32_t chunk = ATA_SECTOR_SIZE - block_off;
        if (chunk > to_read - done) chunk = to_read - done;
        memcpy(buffer + done, sector + block_off, chunk);
        done += chunk;
    }
    return (ssize_t)done;
}

static ssize_t diskfs_op_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buffer) {
    if (!node || node->type != NODE_FILE) return -1;
    if (offset > DISKFS_MAX_FILE_SIZE) return -1;

    diskfs_inode_t *ino = &inode_table[node->inode];
    uint32_t to_write = size;
    if (offset + to_write > DISKFS_MAX_FILE_SIZE) to_write = DISKFS_MAX_FILE_SIZE - offset;

    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < to_write) {
        uint32_t file_pos  = offset + done;
        uint32_t block_idx = file_pos / ATA_SECTOR_SIZE;
        uint32_t block_off = file_pos % ATA_SECTOR_SIZE;
        if (block_idx >= DISKFS_DIRECT_BLOCKS) break;

        if (ino->blocks[block_idx] == DISKFS_NO_BLOCK) {
            uint32_t nb = bitmap_alloc();
            if (nb == DISKFS_NO_BLOCK) break;   /* disk full */
            ino->blocks[block_idx] = nb;
        }

        uint32_t chunk = ATA_SECTOR_SIZE - block_off;
        if (chunk > to_write - done) chunk = to_write - done;

        /* Partial-sector write: read-modify-write so bytes outside this
         * write's range survive. */
        if (block_off != 0 || chunk != ATA_SECTOR_SIZE) {
            memset(sector, 0, sizeof(sector));
            ata_read_sectors(sb.data_start_lba + ino->blocks[block_idx], 1, sector);
        }
        memcpy(sector + block_off, buffer + done, chunk);
        if (ata_write_sectors(sb.data_start_lba + ino->blocks[block_idx], 1, sector) != 0) {
            break;
        }
        done += chunk;
    }

    if (offset + done > ino->size) {
        ino->size = offset + done;
        node->length = ino->size;
    }
    diskfs_flush_bitmap();
    diskfs_flush_inode_table();
    return (ssize_t)done;
}

static void diskfs_op_open(vfs_node_t *node)  { (void)node; }
static void diskfs_op_close(vfs_node_t *node) { (void)node; }

static vfs_dirent_t *diskfs_op_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || node->type != NODE_DIRECTORY) return 0;
    if (index >= node->child_count) return 0;
    vfs_node_t *child = node->children[index];
    if (!child) return 0;
    string_copy(disk_dirent.name, child->name);
    disk_dirent.inode = child->inode;
    return &disk_dirent;
}

static vfs_node_t *diskfs_create_child(vfs_node_t *parent, const char *name, vfs_node_type_t type);
static int diskfs_remove_child(vfs_node_t *parent, const char *name);

static void diskfs_wire_node(vfs_node_t *n) {
    n->read         = diskfs_op_read;
    n->write        = diskfs_op_write;
    n->open         = diskfs_op_open;
    n->close        = diskfs_op_close;
    n->readdir      = diskfs_op_readdir;
    n->create_child = diskfs_create_child;
    n->remove_child = diskfs_remove_child;
}

static vfs_node_t *diskfs_create_child(vfs_node_t *parent, const char *name, vfs_node_type_t type) {
    if (!disk_mounted) return 0;

    uint32_t idx = DISKFS_NO_INODE;
    for (uint32_t i = 1; i < DISKFS_MAX_INODES; i++) {   /* index 0 is always root */
        if (!inode_table[i].used) { idx = i; break; }
    }
    if (idx == DISKFS_NO_INODE) return 0;   /* out of inodes */

    diskfs_inode_t *ino = &inode_table[idx];
    memset(ino, 0, sizeof(*ino));
    ino->used = 1;
    ino->type = (type == NODE_DIRECTORY) ? DISKFS_TYPE_DIR : DISKFS_TYPE_FILE;
    strncpy(ino->name, name, DISKFS_MAX_NAME - 1);
    ino->parent = parent->inode;
    for (int b = 0; b < DISKFS_DIRECT_BLOCKS; b++) ino->blocks[b] = DISKFS_NO_BLOCK;

    vfs_node_t *n = &disk_nodes[idx];
    memset(n, 0, sizeof(*n));
    string_copy(n->name, ino->name);   /* possibly truncated - keep node/inode names identical */
    n->type = type;
    n->inode = idx;
    diskfs_wire_node(n);

    if (vfs_add_child(parent, n) != 0) {
        ino->used = 0;
        return 0;
    }

    diskfs_flush_inode_table();
    return n;
}

static int diskfs_remove_child(vfs_node_t *parent, const char *name) {
    if (!disk_mounted) return -1;

    vfs_node_t *child = vfs_find_node(parent, name);
    if (!child) return -1;

    diskfs_inode_t *ino = &inode_table[child->inode];
    for (int b = 0; b < DISKFS_DIRECT_BLOCKS; b++) {
        if (ino->blocks[b] != DISKFS_NO_BLOCK) {
            bitmap_clear(ino->blocks[b]);
        }
    }
    memset(ino, 0, sizeof(*ino));   /* used = 0, frees the inode slot */

    if (vfs_remove_child(parent, name) != 0) {
        return -1;
    }

    diskfs_flush_bitmap();
    diskfs_flush_inode_table();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Mount                                                                */
/* ------------------------------------------------------------------ */

void diskfs_init(void) {
    if (!ata_is_present()) {
        console_write("DISKFS: no disk detected, /mnt not mounted\n");
        return;
    }

    if (diskfs_load_superblock() != 0) {
        console_write("DISKFS: no valid OrgitoFS superblock found, formatting...\n");
        diskfs_format();
    } else {
        console_write("DISKFS: valid OrgitoFS superblock found, mounting...\n");
    }

    diskfs_load_bitmap_and_inodes();

    /* Pass 1: materialize every used inode as a disk_nodes[] slot
     * (name/type/inode/ops), independent of tree position. */
    for (uint32_t i = 0; i < DISKFS_MAX_INODES; i++) {
        if (!inode_table[i].used) continue;
        vfs_node_t *n = &disk_nodes[i];
        memset(n, 0, sizeof(*n));
        string_copy(n->name, inode_table[i].name);
        n->type   = (inode_table[i].type == DISKFS_TYPE_DIR) ? NODE_DIRECTORY : NODE_FILE;
        n->inode  = i;
        n->length = inode_table[i].size;
        diskfs_wire_node(n);
    }

    /* Pass 2: link parent<->children via the same generic vfs_add_child()
     * ramfs uses - see the file header comment for why that's safe and
     * sufficient (no on-disk directory-entry blocks needed). Root (0) is
     * excluded here; it gets linked into the real VFS tree below instead. */
    for (uint32_t i = 0; i < DISKFS_MAX_INODES; i++) {
        if (i == DISKFS_ROOT_INODE || !inode_table[i].used) continue;
        uint32_t p = inode_table[i].parent;
        if (p >= DISKFS_MAX_INODES || !inode_table[p].used) continue;   /* orphaned - skip */
        vfs_add_child(&disk_nodes[p], &disk_nodes[i]);
    }

    /* Mount the disk's root directory as /mnt under the real VFS root. */
    if (vfs_add_child(vfs_get_root(), &disk_nodes[DISKFS_ROOT_INODE]) != 0) {
        console_write("DISKFS: failed to mount /mnt (root directory full)\n");
        return;
    }

    disk_mounted = 1;
    console_write("DISKFS: mounted at /mnt\n");
}
