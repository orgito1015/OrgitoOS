/*
 * OrgitoOS - Virtual File System
 *
 * Provides a unified interface for file operations across different filesystems.
 *
 * Two backends plug into the same vfs_node_t tree:
 *   - ramfs (fs/vfs.c): the in-memory tree mounted at /. File data lives in
 *     a heap buffer that grows on demand (up to VFS_MAX_FILE_SIZE).
 *   - OrgitoFS (fs/diskfs.c): the persistent on-disk filesystem mounted at
 *     /mnt. Its directories set the create_child/remove_child/truncate hooks
 *     so that creating, deleting or truncating anything under /mnt goes to
 *     the disk.
 *
 * Code outside fs/ must therefore create, delete and truncate files only
 * through vfs_create_in(), vfs_unlink() and vfs_truncate(), never by
 * touching node fields directly.
 */

#ifndef ORGITOOS_FS_VFS_H
#define ORGITOOS_FS_VFS_H

#include <stdint.h>
#include <stddef.h>

/* Define ssize_t if not available */
#ifndef SSIZE_MAX
typedef int32_t ssize_t;
#endif

/* Maximum limits */
#define VFS_MAX_NODES       256
#define VFS_MAX_CHILDREN    32
#define VFS_MAX_FILE_SIZE   (1024u * 1024u)   /* 1 MiB per ramfs file */
#define VFS_NAME_LENGTH     128
#define VFS_MAX_PATH_LENGTH 256

/* Node types */
typedef enum {
    NODE_FILE,
    NODE_DIRECTORY
} vfs_node_type_t;

/* Forward declarations */
struct vfs_node;
struct vfs_dirent;

/* Directory entry structure */
typedef struct vfs_dirent {
    char name[VFS_NAME_LENGTH];
    uint32_t inode;
} vfs_dirent_t;

/* VFS node structure */
typedef struct vfs_node {
    char name[VFS_NAME_LENGTH];
    vfs_node_type_t type;
    uint32_t inode;
    uint32_t length;
    uint32_t flags;              /* permission bits, see kernel/fileio.h */
    struct vfs_node* parent;
    struct vfs_node* children[VFS_MAX_CHILDREN];
    uint32_t child_count;

    /* ramfs file data: heap buffer of `capacity` bytes (NULL until the
     * first write). Unused by disk-backed nodes. */
    uint8_t *content;
    uint32_t capacity;

    /* Function pointers for operations */
    ssize_t (*read)(struct vfs_node*, uint32_t, uint32_t, uint8_t*);
    ssize_t (*write)(struct vfs_node*, uint32_t, uint32_t, const uint8_t*);
    void (*open)(struct vfs_node*);
    void (*close)(struct vfs_node*);
    struct vfs_dirent* (*readdir)(struct vfs_node*, uint32_t);

    /* Directory hooks (NULL = ramfs behaviour). Set by fs/diskfs.c. */
    struct vfs_node* (*create_child)(struct vfs_node* dir, const char* name,
                                     vfs_node_type_t type);
    int (*remove_child)(struct vfs_node* dir, const char* name);
    /* File hook: shrink/grow logical size (NULL = ramfs behaviour). */
    int (*truncate)(struct vfs_node* file, uint32_t length);
} vfs_node_t;

/* VFS initialization (call after heap_init: file data lives on the heap) */
void vfs_init(void);

/* Core VFS functions */
vfs_node_t* vfs_get_root(void);
vfs_node_t* vfs_create_node(const char* name, vfs_node_type_t type);
vfs_node_t* vfs_find_node(vfs_node_t* parent, const char* name);
int vfs_add_child(vfs_node_t* parent, vfs_node_t* child);
int vfs_remove_child(vfs_node_t* parent, const char* name);
vfs_node_t* vfs_resolve_path(const char* path);

/* Filesystem-aware helpers: honour the parent's backend (ramfs or disk).
 * Use these instead of vfs_create_node()+vfs_add_child() /
 * vfs_remove_child() / writing node->length. */
vfs_node_t* vfs_create_in(vfs_node_t* parent, const char* name, vfs_node_type_t type);
int vfs_unlink(vfs_node_t* parent, const char* name);
int vfs_truncate(vfs_node_t* file, uint32_t length);

/* File operations */
ssize_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
ssize_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
vfs_dirent_t* vfs_readdir(vfs_node_t* node, uint32_t index);

/* Utility functions */
void vfs_list_directory(vfs_node_t* dir);

#endif /* ORGITOOS_FS_VFS_H */
