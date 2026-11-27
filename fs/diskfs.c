/*
 * OrgitoOS - OrgitoFS: a small custom on-disk filesystem
 *
 * Layout on the ATA disk (drivers/ata.c), all offsets in 512-byte
 * sectors:
 *
 *   LBA 0                            superblock (1 sector)
 *   LBA 1 .. +inode_table_sectors    flat inode table (fixed-size array)
 *   LBA .. +bitmap_sectors           block bitmap (1 bit per data block)
 *   LBA data_start_lba ..            data blocks (1 block = 1 sector)
 *
 * Inodes: DISKFS_MAX_INODES entries, each storing name, type, size,
 * permission bits, parent inode index, DISKFS_DIRECT_BLOCKS direct block
 * numbers and one single-indirect block (a sector holding 128 more block
 * numbers). Maximum file size: (12 + 128) * 512 = 71,680 bytes.
 *
 * Directories have no on-disk entry blocks: each inode stores its
 * parent's index, and the in-memory vfs_node_t tree is rebuilt from that
 * at mount time with the generic vfs_add_child(). The disk root (inode 0)
 * is mounted at /mnt.
 *
 * Every change is written through immediately (no cache, no `sync`
 * needed): after a write, only the inode's own sector(s) and, if it
 * changed, the block bitmap are written back.
 *
 * The VFS reaches this code through the node hooks set in
 * diskfs_wire_node(): read/write/readdir for data, and create_child /
 * remove_child / truncate, which fs/vfs.c's vfs_create_in(), vfs_unlink()
 * and vfs_truncate() call for anything under /mnt.
 */

#include "diskfs.h"
#include "vfs.h"
#include "../drivers/ata.h"
#include "../drivers/console.h"
#include "../kernel/string.h"

#define DISKFS_MAGIC          0x4F524732u   /* "ORG2" (v2 layout) */
#define DISKFS_MAX_INODES     128
#define DISKFS_MAX_NAME       32
#define DISKFS_DIRECT_BLOCKS  12
#define DISKFS_PTRS_PER_BLOCK (ATA_SECTOR_SIZE / 4)          /* 128 */
#define DISKFS_NO_INODE       0xFFFFFFFFu
#define DISKFS_NO_BLOCK       0xFFFFFFFFu
#define DISKFS_TYPE_FILE      0
#define DISKFS_TYPE_DIR       1
#define DISKFS_ROOT_INODE     0u

#define DISKFS_MAX_FILE_BLOCKS (DISKFS_DIRECT_BLOCKS + DISKFS_PTRS_PER_BLOCK)
#define DISKFS_MAX_FILE_SIZE   (DISKFS_MAX_FILE_BLOCKS * ATA_SECTOR_SIZE)

/* Fixed bound on the block bitmap: 4096 bytes * 8 bits * 512 B/block
 * = 16 MiB of data blocks. */
#define DISKFS_MAX_BITMAP_BYTES 4096u

/* Permission bits are stored on disk; same encoding as vfs_node_t.flags
 * (kernel/fileio.h: bit 31 = "mode set", low 9 bits = rwxrwxrwx). */
#define DISKFS_FLAG_MODE_SET 0x80000000u

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
    uint16_t mode;                           /* rwxrwxrwx, 0 = default */
    char     name[DISKFS_MAX_NAME];
    uint32_t size;
    uint32_t parent;
    uint32_t blocks[DISKFS_DIRECT_BLOCKS];
    uint32_t indirect;                       /* block of 128 more pointers */
} diskfs_inode_t;

static diskfs_superblock_t sb;
static diskfs_inode_t inode_table[DISKFS_MAX_INODES];
static uint8_t block_bitmap[DISKFS_MAX_BITMAP_BYTES];
static int bitmap_dirty = 0;

/* disk_nodes[i] is the in-memory vfs_node_t for inode_table[i]; a node's
 * `inode` field is its index in both arrays. */
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
    bitmap_dirty = 1;
}
static void bitmap_clear(uint32_t bit) {
    block_bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    bitmap_dirty = 1;
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
    if (!bitmap_dirty) return;
    for (uint32_t s = 0; s < sb.bitmap_sectors; s++) {
        ata_write_sectors(sb.bitmap_start_lba + s, 1, block_bitmap + (size_t)s * ATA_SECTOR_SIZE);
    }
    bitmap_dirty = 0;
}

/* Write inode-table sectors [first, last] back to disk. */
static void flush_inode_sectors(uint32_t first, uint32_t last) {
    uint8_t sector[ATA_SECTOR_SIZE];
    const uint8_t *src = (const uint8_t *)inode_table;
    uint32_t total_bytes = (uint32_t)sizeof(inode_table);
    for (uint32_t s = first; s <= last && s < sb.inode_table_sectors; s++) {
        uint32_t off = s * ATA_SECTOR_SIZE;
        uint32_t chunk = (total_bytes > off) ? (total_bytes - off) : 0;
        if (chunk > ATA_SECTOR_SIZE) chunk = ATA_SECTOR_SIZE;
        memset(sector, 0, sizeof(sector));
        if (chunk > 0) memcpy(sector, src + off, chunk);
        ata_write_sectors(sb.inode_table_start_lba + s, 1, sector);
    }
}

/* Copy a node's in-memory permission bits into its inode. */
static void capture_mode(uint32_t idx) {
    uint32_t f = disk_nodes[idx].flags;
    inode_table[idx].mode = (f & DISKFS_FLAG_MODE_SET) ? (uint16_t)(f & 0x1FFu) : 0;
}

/* Write one inode back (only the sector(s) it occupies). */
static void diskfs_flush_inode(uint32_t idx) {
    capture_mode(idx);
    uint32_t start = idx * (uint32_t)sizeof(diskfs_inode_t);
    uint32_t end   = start + (uint32_t)sizeof(diskfs_inode_t) - 1;
    flush_inode_sectors(start / ATA_SECTOR_SIZE, end / ATA_SECTOR_SIZE);
}

static void diskfs_flush_inode_table(void) {
    for (uint32_t i = 0; i < DISKFS_MAX_INODES; i++) {
        if (inode_table[i].used) capture_mode(i);
    }
    flush_inode_sectors(0, sb.inode_table_sectors - 1);
}

static int diskfs_load_superblock(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    if (ata_read_sectors(0, 1, sector) != 0) {
        return -1;
    }
    memcpy(&sb, sector, sizeof(sb));
    return (sb.magic == DISKFS_MAGIC && sb.inode_count == DISKFS_MAX_INODES) ? 0 : -1;
}

static void diskfs_load_bitmap_and_inodes(void) {
    for (uint32_t s = 0; s < sb.bitmap_sectors && (size_t)s * ATA_SECTOR_SIZE < DISKFS_MAX_BITMAP_BYTES; s++) {
        ata_read_sectors(sb.bitmap_start_lba + s, 1, block_bitmap + (size_t)s * ATA_SECTOR_SIZE);
    }
    bitmap_dirty = 0;

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

static void init_inode_blocks(diskfs_inode_t *ino) {
    for (int b = 0; b < DISKFS_DIRECT_BLOCKS; b++) ino->blocks[b] = DISKFS_NO_BLOCK;
    ino->indirect = DISKFS_NO_BLOCK;
}

/* Formats a brand-new, empty OrgitoFS sized to the attached disk, with a
 * single root directory inode (index 0, mounted as /mnt). */
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
    memset(disk_nodes, 0, sizeof(disk_nodes));

    diskfs_inode_t *root = &inode_table[DISKFS_ROOT_INODE];
    root->used = 1;
    root->type = DISKFS_TYPE_DIR;
    strncpy(root->name, "mnt", DISKFS_MAX_NAME - 1);
    root->parent = DISKFS_ROOT_INODE;   /* self-parented, like the ramfs root */
    init_inode_blocks(root);

    diskfs_flush_superblock();
    bitmap_dirty = 1;
    diskfs_flush_bitmap();
    flush_inode_sectors(0, sb.inode_table_sectors - 1);

    console_write("DISKFS: formatted a fresh OrgitoFS (");
    { char buf[12]; itoa((int)sb.total_blocks, buf, 10); console_write(buf); }
    console_write(" blocks, ");
    { char buf[12]; itoa((int)(sb.total_blocks / 2048), buf, 10); console_write(buf); }
    console_write(" MiB usable)\n");
}

/* ------------------------------------------------------------------ */
/* Block mapping (direct + single indirect)                             */
/* ------------------------------------------------------------------ */

/* Block number holding file block `n` of `ino`, or DISKFS_NO_BLOCK. With
 * `alloc`, missing blocks (and the indirect block) are allocated. */
static uint32_t inode_block(diskfs_inode_t *ino, uint32_t n, int alloc) {
    if (n < DISKFS_DIRECT_BLOCKS) {
        if (ino->blocks[n] == DISKFS_NO_BLOCK && alloc) {
            ino->blocks[n] = bitmap_alloc();
        }
        return ino->blocks[n];
    }
    n -= DISKFS_DIRECT_BLOCKS;
    if (n >= DISKFS_PTRS_PER_BLOCK) {
        return DISKFS_NO_BLOCK;
    }

    uint32_t ptrs[DISKFS_PTRS_PER_BLOCK];
    if (ino->indirect == DISKFS_NO_BLOCK) {
        if (!alloc) return DISKFS_NO_BLOCK;
        uint32_t ib = bitmap_alloc();
        if (ib == DISKFS_NO_BLOCK) return DISKFS_NO_BLOCK;
        for (uint32_t i = 0; i < DISKFS_PTRS_PER_BLOCK; i++) ptrs[i] = DISKFS_NO_BLOCK;
        if (ata_write_sectors(sb.data_start_lba + ib, 1, ptrs) != 0) {
            bitmap_clear(ib);
            return DISKFS_NO_BLOCK;
        }
        ino->indirect = ib;
    } else if (ata_read_sectors(sb.data_start_lba + ino->indirect, 1, ptrs) != 0) {
        return DISKFS_NO_BLOCK;
    }

    if (ptrs[n] == DISKFS_NO_BLOCK && alloc) {
        uint32_t nb = bitmap_alloc();
        if (nb == DISKFS_NO_BLOCK) return DISKFS_NO_BLOCK;
        ptrs[n] = nb;
        if (ata_write_sectors(sb.data_start_lba + ino->indirect, 1, ptrs) != 0) {
            bitmap_clear(nb);
            return DISKFS_NO_BLOCK;
        }
    }
    return ptrs[n];
}

/* Free every block from file block `from` onwards (truncate / delete). */
static void inode_free_blocks_from(diskfs_inode_t *ino, uint32_t from) {
    for (uint32_t b = from; b < DISKFS_DIRECT_BLOCKS; b++) {
        if (ino->blocks[b] != DISKFS_NO_BLOCK) {
            bitmap_clear(ino->blocks[b]);
            ino->blocks[b] = DISKFS_NO_BLOCK;
        }
    }
    if (ino->indirect == DISKFS_NO_BLOCK) {
        return;
    }
    uint32_t ptrs[DISKFS_PTRS_PER_BLOCK];
    if (ata_read_sectors(sb.data_start_lba + ino->indirect, 1, ptrs) != 0) {
        return;
    }
    uint32_t first = (from > DISKFS_DIRECT_BLOCKS) ? from - DISKFS_DIRECT_BLOCKS : 0;
    int any_left = 0;
    for (uint32_t i = 0; i < DISKFS_PTRS_PER_BLOCK; i++) {
        if (ptrs[i] == DISKFS_NO_BLOCK) continue;
        if (i >= first) {
            bitmap_clear(ptrs[i]);
            ptrs[i] = DISKFS_NO_BLOCK;
        } else {
            any_left = 1;
        }
    }
    if (any_left) {
        ata_write_sectors(sb.data_start_lba + ino->indirect, 1, ptrs);
    } else {
        bitmap_clear(ino->indirect);
        ino->indirect = DISKFS_NO_BLOCK;
    }
}

/* ------------------------------------------------------------------ */
/* vfs_node_t operations                                                */
/* ------------------------------------------------------------------ */

static ssize_t diskfs_op_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || node->type != NODE_FILE) return -1;
    diskfs_inode_t *ino = &inode_table[node->inode];

    if (offset >= ino->size) return 0;
    uint32_t to_read = size;
    if (to_read > ino->size - offset) to_read = ino->size - offset;

    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < to_read) {
        uint32_t file_pos  = offset + done;
        uint32_t block_off = file_pos % ATA_SECTOR_SIZE;
        uint32_t blk = inode_block(ino, file_pos / ATA_SECTOR_SIZE, 0);
        uint32_t chunk = ATA_SECTOR_SIZE - block_off;
        if (chunk > to_read - done) chunk = to_read - done;

        if (blk == DISKFS_NO_BLOCK) {
            memset(buffer + done, 0, chunk);          /* hole reads as zeros */
        } else {
            if (ata_read_sectors(sb.data_start_lba + blk, 1, sector) != 0) break;
            memcpy(buffer + done, sector + block_off, chunk);
        }
        done += chunk;
    }
    return (ssize_t)done;
}

/* Writes what fits: at most DISKFS_MAX_FILE_SIZE per file and only while
 * there are free blocks. A short count is a partial write; 0 means
 * nothing could be stored (disk or file full). */
static ssize_t diskfs_op_write(vfs_node_t *node, uint32_t offset, uint32_t size, const uint8_t *buffer) {
    if (!node || node->type != NODE_FILE) return -1;
    if (offset >= DISKFS_MAX_FILE_SIZE) return 0;

    diskfs_inode_t *ino = &inode_table[node->inode];
    uint32_t to_write = size;
    if (to_write > DISKFS_MAX_FILE_SIZE - offset) to_write = DISKFS_MAX_FILE_SIZE - offset;

    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t done = 0;
    while (done < to_write) {
        uint32_t file_pos  = offset + done;
        uint32_t block_off = file_pos % ATA_SECTOR_SIZE;
        uint32_t blk = inode_block(ino, file_pos / ATA_SECTOR_SIZE, 1);
        if (blk == DISKFS_NO_BLOCK) break;            /* disk full */

        uint32_t chunk = ATA_SECTOR_SIZE - block_off;
        if (chunk > to_write - done) chunk = to_write - done;

        /* Partial sector: read-modify-write so the other bytes survive. */
        if (block_off != 0 || chunk != ATA_SECTOR_SIZE) {
            memset(sector, 0, sizeof(sector));
            if (file_pos - block_off < ino->size) {
                ata_read_sectors(sb.data_start_lba + blk, 1, sector);
            }
        }
        memcpy(sector + block_off, buffer + done, chunk);
        if (ata_write_sectors(sb.data_start_lba + blk, 1, sector) != 0) break;
        done += chunk;
    }

    if (offset + done > ino->size) {
        ino->size = offset + done;
        node->length = ino->size;
    }
    diskfs_flush_bitmap();
    diskfs_flush_inode(node->inode);
    return (ssize_t)done;
}

static int diskfs_op_truncate(vfs_node_t *node, uint32_t length) {
    if (!node || node->type != NODE_FILE) return -1;
    if (length > DISKFS_MAX_FILE_SIZE) return -1;
    diskfs_inode_t *ino = &inode_table[node->inode];

    if (length < ino->size) {
        uint32_t keep_blocks = (length + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
        inode_free_blocks_from(ino, keep_blocks);
        /* Zero the tail of the last kept block so a later grow reads 0s. */
        if (length % ATA_SECTOR_SIZE) {
            uint32_t blk = inode_block(ino, length / ATA_SECTOR_SIZE, 0);
            uint8_t sector[ATA_SECTOR_SIZE];
            if (blk != DISKFS_NO_BLOCK &&
                ata_read_sectors(sb.data_start_lba + blk, 1, sector) == 0) {
                memset(sector + length % ATA_SECTOR_SIZE, 0,
                       ATA_SECTOR_SIZE - length % ATA_SECTOR_SIZE);
                ata_write_sectors(sb.data_start_lba + blk, 1, sector);
            }
        }
    }
    /* Growing just raises the size: missing blocks read back as zeros. */
    ino->size = length;
    node->length = length;
    diskfs_flush_bitmap();
    diskfs_flush_inode(node->inode);
    return 0;
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
    n->truncate     = diskfs_op_truncate;
    if (n->type == NODE_DIRECTORY) {
        n->create_child = diskfs_create_child;
        n->remove_child = diskfs_remove_child;
    }
}

static vfs_node_t *diskfs_create_child(vfs_node_t *parent, const char *name, vfs_node_type_t type) {
    if (!disk_mounted) return 0;
    if (string_length(name) >= DISKFS_MAX_NAME) return 0;   /* name too long */
    if (parent->child_count >= VFS_MAX_CHILDREN) return 0;

    uint32_t idx = DISKFS_NO_INODE;
    for (uint32_t i = 1; i < DISKFS_MAX_INODES; i++) {   /* 0 is the root */
        if (!inode_table[i].used) { idx = i; break; }
    }
    if (idx == DISKFS_NO_INODE) return 0;   /* out of inodes */

    diskfs_inode_t *ino = &inode_table[idx];
    memset(ino, 0, sizeof(*ino));
    ino->used = 1;
    ino->type = (type == NODE_DIRECTORY) ? DISKFS_TYPE_DIR : DISKFS_TYPE_FILE;
    strncpy(ino->name, name, DISKFS_MAX_NAME - 1);
    ino->parent = parent->inode;
    init_inode_blocks(ino);

    vfs_node_t *n = &disk_nodes[idx];
    memset(n, 0, sizeof(*n));
    string_copy(n->name, ino->name);
    n->type = type;
    n->inode = idx;
    diskfs_wire_node(n);

    if (vfs_add_child(parent, n) != 0) {
        ino->used = 0;
        return 0;
    }
    diskfs_flush_inode(idx);
    return n;
}

static int diskfs_remove_child(vfs_node_t *parent, const char *name) {
    if (!disk_mounted) return -1;

    vfs_node_t *child = vfs_find_node(parent, name);
    if (!child) return -1;
    if (child->type == NODE_DIRECTORY && child->child_count > 0) {
        return -1;   /* directory not empty */
    }

    uint32_t idx = child->inode;
    diskfs_inode_t *ino = &inode_table[idx];
    inode_free_blocks_from(ino, 0);
    memset(ino, 0, sizeof(*ino));   /* used = 0: the inode slot is free */

    if (vfs_remove_child(parent, name) != 0) {
        return -1;
    }
    memset(&disk_nodes[idx], 0, sizeof(disk_nodes[idx]));

    diskfs_flush_bitmap();
    diskfs_flush_inode(idx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public                                                               */
/* ------------------------------------------------------------------ */

void diskfs_sync(void) {
    if (!disk_mounted) return;
    diskfs_flush_bitmap();
    diskfs_flush_inode_table();
}

int diskfs_is_mounted(void) {
    return disk_mounted;
}

void diskfs_stats(uint32_t *total_blocks, uint32_t *free_blocks,
                  uint32_t *total_inodes, uint32_t *free_inodes) {
    uint32_t fb = 0, fi = 0;
    if (disk_mounted) {
        for (uint32_t i = 0; i < sb.total_blocks; i++) if (!bitmap_test(i)) fb++;
        for (uint32_t i = 0; i < DISKFS_MAX_INODES; i++) if (!inode_table[i].used) fi++;
    }
    if (total_blocks) *total_blocks = disk_mounted ? sb.total_blocks : 0;
    if (free_blocks)  *free_blocks  = fb;
    if (total_inodes) *total_inodes = disk_mounted ? DISKFS_MAX_INODES : 0;
    if (free_inodes)  *free_inodes  = fi;
}

uint32_t diskfs_max_file_size(void) {
    return DISKFS_MAX_FILE_SIZE;
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
        console_write("DISKFS: no valid OrgitoFS found, formatting the disk...\n");
        diskfs_format();
    }

    diskfs_load_bitmap_and_inodes();

    /* Pass 1: a vfs_node_t for every used inode. */
    uint32_t files = 0;
    for (uint32_t i = 0; i < DISKFS_MAX_INODES; i++) {
        if (!inode_table[i].used) continue;
        vfs_node_t *n = &disk_nodes[i];
        memset(n, 0, sizeof(*n));
        inode_table[i].name[DISKFS_MAX_NAME - 1] = '\0';
        string_copy(n->name, inode_table[i].name);
        n->type   = (inode_table[i].type == DISKFS_TYPE_DIR) ? NODE_DIRECTORY : NODE_FILE;
        n->inode  = i;
        n->length = inode_table[i].size;
        if (inode_table[i].mode) {
            n->flags = DISKFS_FLAG_MODE_SET | inode_table[i].mode;
        }
        diskfs_wire_node(n);
        if (i != DISKFS_ROOT_INODE) files++;
    }

    /* Pass 2: link children to parents. */
    for (uint32_t i = 0; i < DISKFS_MAX_INODES; i++) {
        if (i == DISKFS_ROOT_INODE || !inode_table[i].used) continue;
        uint32_t p = inode_table[i].parent;
        if (p >= DISKFS_MAX_INODES || !inode_table[p].used ||
            inode_table[p].type != DISKFS_TYPE_DIR) continue;   /* orphan */
        vfs_add_child(&disk_nodes[p], &disk_nodes[i]);
    }

    /* Mount the disk root as /mnt. */
    if (vfs_add_child(vfs_get_root(), &disk_nodes[DISKFS_ROOT_INODE]) != 0) {
        console_write("DISKFS: failed to mount /mnt (root directory full)\n");
        return;
    }

    disk_mounted = 1;
    console_write("DISKFS: OrgitoFS mounted at /mnt (");
    { char buf[12]; itoa((int)files, buf, 10); console_write(buf); }
    console_write(" files/directories)\n");
}
