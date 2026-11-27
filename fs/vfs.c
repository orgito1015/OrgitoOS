/*
 * OrgitoOS - Virtual File System Implementation
 *
 * RAM-based filesystem (ramfs) plus the backend-neutral VFS helpers.
 *
 * Nodes come from a static pool; file *data* lives in a heap buffer that
 * grows on demand (doubling, up to VFS_MAX_FILE_SIZE), so files are no
 * longer limited to a fixed 4 KiB array and an empty file costs nothing.
 */

#include "vfs.h"
#include "../kernel/string.h"
#include "../drivers/console.h"
#include "../memory/heap.h"

/* Static memory pool for VFS nodes */
static vfs_node_t node_pool[VFS_MAX_NODES];
static int node_pool_bitmap[VFS_MAX_NODES];  /* 0 = free, 1 = used */
static uint32_t next_inode = 1;

/* Root directory pointer */
static vfs_node_t* vfs_root = 0;

/* Static dirent for readdir operations */
static vfs_dirent_t static_dirent;

/* Forward declarations for ramfs operations */
static ssize_t ramfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
static ssize_t ramfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
static void ramfs_open(vfs_node_t* node);
static void ramfs_close(vfs_node_t* node);
static vfs_dirent_t* ramfs_readdir(vfs_node_t* node, uint32_t index);

/*
 * Allocate a node from the static pool
 */
static vfs_node_t* allocate_node(void) {
    for (int i = 0; i < VFS_MAX_NODES; i++) {
        if (node_pool_bitmap[i] == 0) {
            node_pool_bitmap[i] = 1;

            vfs_node_t* node = &node_pool[i];
            memset(node, 0, sizeof(*node));
            node->type  = NODE_FILE;
            node->inode = next_inode++;

            /* Set up operation function pointers */
            node->read    = ramfs_read;
            node->write   = ramfs_write;
            node->open    = ramfs_open;
            node->close   = ramfs_close;
            node->readdir = ramfs_readdir;
            return node;
        }
    }
    return 0;  /* Out of nodes */
}

/*
 * Return a node to the pool (no-op for nodes that are not from the pool,
 * e.g. disk-backed nodes owned by fs/diskfs.c).
 */
static void free_node(vfs_node_t* node) {
    for (int i = 0; i < VFS_MAX_NODES; i++) {
        if (&node_pool[i] == node) {
            if (node->content) {
                kfree(node->content);
            }
            node->content  = 0;
            node->capacity = 0;
            node_pool_bitmap[i] = 0;
            return;
        }
    }
}

/* Make sure a ramfs file can hold `needed` bytes. Returns 0 or -1. */
static int ramfs_reserve(vfs_node_t* node, uint32_t needed) {
    if (needed <= node->capacity) {
        return 0;
    }
    if (needed > VFS_MAX_FILE_SIZE) {
        return -1;
    }
    uint32_t cap = node->capacity ? node->capacity : 256;
    while (cap < needed) {
        cap *= 2;
    }
    if (cap > VFS_MAX_FILE_SIZE) {
        cap = VFS_MAX_FILE_SIZE;
    }
    uint8_t* buf = (uint8_t*)kmalloc(cap);
    if (!buf) {
        return -1;
    }
    if (node->content) {
        memcpy(buf, node->content, node->length);
        kfree(node->content);
    }
    node->content  = buf;
    node->capacity = cap;
    return 0;
}

/*
 * Read from a ramfs file
 */
static ssize_t ramfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || node->type != NODE_FILE) {
        return -1;
    }
    if (offset >= node->length) {
        return 0;  /* EOF */
    }
    uint32_t n = size;
    if (n > node->length - offset) {
        n = node->length - offset;
    }
    memcpy(buffer, node->content + offset, n);
    return (ssize_t)n;
}

/*
 * Write to a ramfs file. Writes past VFS_MAX_FILE_SIZE are cut short
 * (a partial write); a write that cannot store a single byte returns 0.
 */
static ssize_t ramfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || node->type != NODE_FILE) {
        return -1;
    }
    if (offset >= VFS_MAX_FILE_SIZE) {
        return 0;
    }
    if (size > VFS_MAX_FILE_SIZE - offset) {
        size = VFS_MAX_FILE_SIZE - offset;
    }
    if (size == 0) {
        return 0;
    }
    if (ramfs_reserve(node, offset + size) != 0) {
        return -1;    /* out of kernel heap */
    }
    /* Writing past EOF leaves a hole: fill it with zeros. */
    if (offset > node->length) {
        memset(node->content + node->length, 0, offset - node->length);
    }
    memcpy(node->content + offset, buffer, size);
    if (offset + size > node->length) {
        node->length = offset + size;
    }
    return (ssize_t)size;
}

static void ramfs_open(vfs_node_t* node) {
    (void)node;
}

static void ramfs_close(vfs_node_t* node) {
    (void)node;
}

/*
 * Read directory entry
 */
static vfs_dirent_t* ramfs_readdir(vfs_node_t* node, uint32_t index) {
    if (!node || node->type != NODE_DIRECTORY) {
        return 0;
    }
    if (index >= node->child_count) {
        return 0;
    }
    vfs_node_t* child = node->children[index];
    if (!child) {
        return 0;
    }
    string_copy(static_dirent.name, child->name);
    static_dirent.inode = child->inode;
    return &static_dirent;
}

/*
 * Initialize the VFS
 */
void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_NODES; i++) {
        node_pool_bitmap[i] = 0;
    }

    /* Create root directory */
    vfs_root = allocate_node();
    if (!vfs_root) {
        console_write("ERROR: Failed to allocate root directory\n");
        return;
    }

    string_copy(vfs_root->name, "/");
    vfs_root->type = NODE_DIRECTORY;
    vfs_root->parent = vfs_root;  /* Root is its own parent */

    /* Create initial directory structure */
    vfs_node_t* bin = vfs_create_node("bin", NODE_DIRECTORY);
    vfs_node_t* etc = vfs_create_node("etc", NODE_DIRECTORY);
    vfs_node_t* home = vfs_create_node("home", NODE_DIRECTORY);
    vfs_node_t* tmp = vfs_create_node("tmp", NODE_DIRECTORY);

    if (bin) vfs_add_child(vfs_root, bin);
    if (etc) vfs_add_child(vfs_root, etc);
    if (home) vfs_add_child(vfs_root, home);
    if (tmp) vfs_add_child(vfs_root, tmp);

    /* Create sample file in /etc */
    if (etc) {
        vfs_node_t* motd = vfs_create_node("motd.txt", NODE_FILE);
        if (motd) {
            const char* content = "Welcome to OrgitoOS!\nThis is a test file in the filesystem.\n";
            uint32_t len = string_length(content);
            vfs_write(motd, 0, len, (const uint8_t*)content);
            vfs_add_child(etc, motd);
        }
    }
}

vfs_node_t* vfs_get_root(void) {
    return vfs_root;
}

/*
 * Create a new, unattached ramfs node
 */
vfs_node_t* vfs_create_node(const char* name, vfs_node_type_t type) {
    vfs_node_t* node = allocate_node();
    if (!node) {
        return 0;
    }
    strncpy(node->name, name, VFS_NAME_LENGTH - 1);
    node->name[VFS_NAME_LENGTH - 1] = '\0';
    node->type = type;
    return node;
}

/*
 * Find a child node by name
 */
vfs_node_t* vfs_find_node(vfs_node_t* parent, const char* name) {
    if (!parent || parent->type != NODE_DIRECTORY) {
        return 0;
    }
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] && string_compare(parent->children[i]->name, name) == 0) {
            return parent->children[i];
        }
    }
    return 0;
}

/*
 * Add a child node to a directory
 */
int vfs_add_child(vfs_node_t* parent, vfs_node_t* child) {
    if (!parent || !child || parent->type != NODE_DIRECTORY) {
        return -1;
    }
    if (parent->child_count >= VFS_MAX_CHILDREN) {
        return -1;  /* Directory full */
    }
    parent->children[parent->child_count] = child;
    parent->child_count++;
    child->parent = parent;
    return 0;
}

/*
 * Detach a child from a directory and release it if it is a ramfs node.
 * NOTE: does not recursively free the children of a removed directory.
 */
int vfs_remove_child(vfs_node_t* parent, const char* name) {
    if (!parent || parent->type != NODE_DIRECTORY) {
        return -1;
    }
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] && string_compare(parent->children[i]->name, name) == 0) {
            free_node(parent->children[i]);
            for (uint32_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->children[parent->child_count - 1] = 0;
            parent->child_count--;
            return 0;
        }
    }
    return -1;  /* Child not found */
}

/* ------------------------------------------------------------------ */
/* Backend-aware helpers                                               */
/* ------------------------------------------------------------------ */

vfs_node_t* vfs_create_in(vfs_node_t* parent, const char* name, vfs_node_type_t type) {
    if (!parent || parent->type != NODE_DIRECTORY || !name || !name[0]) {
        return 0;
    }
    if (vfs_find_node(parent, name)) {
        return 0;   /* already exists */
    }
    if (parent->create_child) {
        return parent->create_child(parent, name, type);   /* e.g. on disk */
    }
    vfs_node_t* node = vfs_create_node(name, type);
    if (!node) {
        return 0;
    }
    if (vfs_add_child(parent, node) != 0) {
        free_node(node);
        return 0;
    }
    return node;
}

int vfs_unlink(vfs_node_t* parent, const char* name) {
    if (!parent || parent->type != NODE_DIRECTORY) {
        return -1;
    }
    /* Refuse to delete a non-empty directory: ramfs would otherwise lose
     * every node inside it (they stay allocated but unreachable). */
    vfs_node_t* child = vfs_find_node(parent, name);
    if (child && child->type == NODE_DIRECTORY && child->child_count > 0) {
        return -1;
    }
    if (parent->remove_child) {
        return parent->remove_child(parent, name);
    }
    return vfs_remove_child(parent, name);
}

int vfs_truncate(vfs_node_t* file, uint32_t length) {
    if (!file || file->type != NODE_FILE) {
        return -1;
    }
    if (file->truncate) {
        return file->truncate(file, length);
    }
    if (length > file->length) {
        /* Grow with zeros */
        if (ramfs_reserve(file, length) != 0) {
            return -1;
        }
        memset(file->content + file->length, 0, length - file->length);
    }
    file->length = length;
    return 0;
}

/*
 * Resolve an absolute path to a node. Re-entrant: it parses the path in
 * place instead of using string_tokenize(), whose hidden static state
 * would be clobbered when two processes resolve paths at the same time.
 */
vfs_node_t* vfs_resolve_path(const char* path) {
    if (!path || path[0] != '/') {
        return 0;   /* only absolute paths */
    }
    vfs_node_t* current = vfs_root;
    const char* p = path;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[VFS_NAME_LENGTH];
        size_t n = 0;
        while (*p && *p != '/') {
            if (n < sizeof(comp) - 1) comp[n++] = *p;
            p++;
        }
        comp[n] = '\0';

        if (string_compare(comp, ".") == 0) {
            continue;
        }
        if (string_compare(comp, "..") == 0) {
            if (current->parent) current = current->parent;
            continue;
        }
        vfs_node_t* next = vfs_find_node(current, comp);
        if (!next) {
            return 0;
        }
        current = next;
    }
    return current;
}

/*
 * Read from a file
 */
ssize_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->read) {
        return -1;
    }
    return node->read(node, offset, size, buffer);
}

/*
 * Write to a file
 */
ssize_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || !node->write) {
        return -1;
    }
    return node->write(node, offset, size, buffer);
}

/*
 * Read directory entry
 */
vfs_dirent_t* vfs_readdir(vfs_node_t* node, uint32_t index) {
    if (!node || !node->readdir) {
        return 0;
    }
    return node->readdir(node, index);
}

/*
 * List directory contents (utility function)
 */
void vfs_list_directory(vfs_node_t* dir) {
    if (!dir || dir->type != NODE_DIRECTORY) {
        console_write("Not a directory\n");
        return;
    }
    for (uint32_t i = 0; i < dir->child_count; i++) {
        vfs_node_t* child = dir->children[i];
        if (child) {
            console_write(child->name);
            if (child->type == NODE_DIRECTORY) {
                console_write("/");
            }
            console_write("\n");
        }
    }
}
