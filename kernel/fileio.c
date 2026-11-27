/*
 * OrgitoOS - POSIX-style file I/O system calls
 *
 * Gives programs the classic Unix file API on top of the in-memory VFS:
 *
 *   fd = open(path, flags, mode)   -> smallest free descriptor (>= 3)
 *   n  = read(fd, buf, count)      -> bytes read, 0 at end of file
 *   n  = write(fd, buf, count)     -> bytes written (may be < count)
 *   close(fd)
 *
 * Design:
 *   - A global open-file table (OFT) holds one entry per successful open():
 *     the VFS node, the current file offset and the open flags.
 *   - Each process has its own descriptor table (process_t.fd_table) that
 *     maps small integers to OFT entries. 0/1/2 are fixed to the keyboard
 *     and the console, so the first file a program opens gets fd 3 - the
 *     same numbering students see under Linux.
 *   - Errors are returned as -errno with Linux's numeric values; the user
 *     library (include/ulibc.h) turns them into the usual "-1 + errno".
 *   - O_CREAT|O_EXCL is checked atomically with interrupts disabled, so an
 *     existing destination file is never overwritten.
 *   - A file has a maximum size (ramfs: VFS_MAX_FILE_SIZE; disk: see
 *     fs/diskfs.c), so a write() near the cap is *partial* (returns fewer
 *     bytes than requested) - exactly the case a correct copy loop must
 *     handle.
 *
 * The bottom of the file implements the syscall tracer behind the `strace`
 * shell command: after each traced syscall the dispatcher calls
 * fileio_trace_*(), which formats a line in strace's own format, e.g.
 *     openat(AT_FDCWD, "burimi.txt", O_RDONLY) = 3
 *     read(3, "Une jam Orgito Leka\n", 4096) = 20
 * and appends it to the trace file (strace -o) or prints it.
 */

#include "fileio.h"
#include "kernel.h"
#include "string.h"
#include "../fs/vfs.h"
#include "../process/process.h"
#include "../drivers/console.h"
#include "../drivers/keyboard.h"

/* ------------------------------------------------------------------ */
/* Open-file table                                                      */
/* ------------------------------------------------------------------ */

#define FILEIO_MAX_OPEN 64

typedef struct {
    int         used;        /* reference count: descriptors naming it */
    vfs_node_t *node;
    uint32_t    offset;
    int         flags;
} open_file_t;

static open_file_t oft[FILEIO_MAX_OPEN];

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint32_t f) {
    if (f & 0x200) __asm__ __volatile__("sti" ::: "memory");
}

static open_file_t *fd_to_file(int fd) {
    process_t *p = process_current();
    if (!p || fd < 3 || fd >= PROCESS_MAX_FDS) return 0;
    int slot = p->fd_table[fd];
    if (slot <= 0 || slot > FILEIO_MAX_OPEN) return 0;
    open_file_t *f = &oft[slot - 1];
    return f->used ? f : 0;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                         */
/* ------------------------------------------------------------------ */

/* Build "/a/b" for the current working directory. */
static void cwd_path(char *out, size_t size) {
    vfs_node_t *cur = kernel_get_current_directory();
    char acc[VFS_MAX_PATH_LENGTH];
    char tmp[VFS_MAX_PATH_LENGTH];
    acc[0] = '\0';
    while (cur && cur->parent && cur->parent != cur) {
        size_t nl = string_length(cur->name), al = string_length(acc);
        if (nl + al + 2 > sizeof(tmp)) break;
        string_copy(tmp, "/");
        string_concat(tmp, cur->name);
        string_concat(tmp, acc);
        string_copy(acc, tmp);
        cur = cur->parent;
    }
    if (acc[0] == '\0') string_copy(acc, "/");
    strncpy(out, acc, size - 1);
    out[size - 1] = '\0';
}

/* Turn `path` into an absolute path. Returns 0 or -errno. */
static int make_absolute(const char *path, char *out, size_t size) {
    size_t len = string_length(path);
    if (len == 0) return -ENOENT;
    if (len >= VFS_NAME_LENGTH) return -ENAMETOOLONG;

    if (path[0] == '/') {
        if (len >= size) return -ENAMETOOLONG;
        string_copy(out, path);
        return 0;
    }
    char base[VFS_MAX_PATH_LENGTH];
    cwd_path(base, sizeof(base));
    if (string_length(base) + len + 2 > size) return -ENAMETOOLONG;
    string_copy(out, base);
    if (base[1] != '\0') string_concat(out, "/");
    string_concat(out, path);
    return 0;
}

vfs_node_t *fileio_lookup(const char *path) {
    char abs[VFS_MAX_PATH_LENGTH];
    if (!path || make_absolute(path, abs, sizeof(abs)) != 0) return 0;
    return vfs_resolve_path(abs);
}

/* Split an absolute path into its parent directory node and leaf name. */
static vfs_node_t *parent_of(const char *abs, char *leaf, size_t leaf_size) {
    int last = -1;
    for (int i = 0; abs[i]; i++) if (abs[i] == '/') last = i;
    if (last < 0) return 0;

    strncpy(leaf, abs + last + 1, leaf_size - 1);
    leaf[leaf_size - 1] = '\0';

    if (last == 0) return vfs_get_root();
    char dir[VFS_MAX_PATH_LENGTH];
    int i = 0;
    for (; i < last && i < (int)sizeof(dir) - 1; i++) dir[i] = abs[i];
    dir[i] = '\0';
    return vfs_resolve_path(dir);
}

uint32_t fileio_node_mode(const vfs_node_t *node) {
    if (!node) return 0;
    if (node->flags & VFS_FLAG_MODE_SET) return node->flags & VFS_MODE_MASK;
    return node->type == NODE_DIRECTORY ? 0755u : VFS_DEFAULT_MODE;
}

/* ------------------------------------------------------------------ */
/* open / read / write / close                                          */
/* ------------------------------------------------------------------ */

int fileio_open(const char *path, int flags, uint32_t mode) {
    process_t *p = process_current();
    if (!p) return -EFAULT;
    if (!path) return -EFAULT;

    int acc = flags & O_ACCMODE;
    if (acc == 3) return -EINVAL;
    int wants_read  = (acc == O_RDONLY || acc == O_RDWR);
    int wants_write = (acc == O_WRONLY || acc == O_RDWR);

    char abs[VFS_MAX_PATH_LENGTH];
    int r = make_absolute(path, abs, sizeof(abs));
    if (r < 0) return r;

    /* Find a free descriptor first, so we never create a file we
     * cannot hand back. */
    int fd = -1;
    for (int i = 3; i < PROCESS_MAX_FDS; i++) {
        if (p->fd_table[i] == 0) { fd = i; break; }
    }
    if (fd < 0) return -EMFILE;

    uint32_t irq = irq_save();   /* make lookup+create atomic (O_EXCL) */

    vfs_node_t *node = vfs_resolve_path(abs);
    int created = 0;

    if (node) {
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            irq_restore(irq);
            return -EEXIST;
        }
        if (node->type == NODE_DIRECTORY && wants_write) {
            irq_restore(irq);
            return -EISDIR;
        }
        uint32_t m = fileio_node_mode(node);
        if ((wants_read && !(m & 0400)) || (wants_write && !(m & 0200))) {
            irq_restore(irq);
            return -EACCES;
        }
    } else {
        if (!(flags & O_CREAT)) {
            irq_restore(irq);
            return -ENOENT;
        }
        char leaf[VFS_NAME_LENGTH];
        vfs_node_t *dir = parent_of(abs, leaf, sizeof(leaf));
        if (!dir) { irq_restore(irq); return -ENOENT; }
        if (dir->type != NODE_DIRECTORY) { irq_restore(irq); return -ENOTDIR; }
        if (leaf[0] == '\0') { irq_restore(irq); return -ENOENT; }
        if (!(fileio_node_mode(dir) & 0200)) { irq_restore(irq); return -EACCES; }

        node = vfs_create_in(dir, leaf, NODE_FILE);
        if (!node) { irq_restore(irq); return -ENOSPC; }
        node->flags = VFS_FLAG_MODE_SET | (mode & VFS_MODE_MASK);
        created = 1;
    }

    int slot = -1;
    for (int i = 0; i < FILEIO_MAX_OPEN; i++) {
        if (!oft[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        if (created) vfs_unlink(node->parent, node->name);
        irq_restore(irq);
        return -ENFILE;
    }

    if ((flags & O_TRUNC) && wants_write && node->type == NODE_FILE) {
        vfs_truncate(node, 0);
    }

    oft[slot].used   = 1;
    oft[slot].node   = node;
    oft[slot].offset = 0;
    oft[slot].flags  = flags;
    p->fd_table[fd]  = (int16_t)(slot + 1);

    irq_restore(irq);
    return fd;
}

int fileio_read(int fd, void *buf, uint32_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;

    if (fd == STDIN_FILENO) {
        /* Terminal semantics: one line per read(), including its '\n'. */
        char line[256];
        uint32_t max = count < sizeof(line) ? count : sizeof(line);
        keyboard_get_line(line, max);         /* leaves room for '\n' */
        uint32_t n = (uint32_t)string_length(line);
        if (n < count) line[n++] = '\n';
        memcpy(buf, line, n);
        return (int)n;
    }
    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) return -EBADF;

    open_file_t *f = fd_to_file(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->node->type == NODE_DIRECTORY) return -EISDIR;

    ssize_t n = vfs_read(f->node, f->offset, count, (uint8_t *)buf);
    if (n < 0) return -EIO;
    f->offset += (uint32_t)n;
    return (int)n;
}

int fileio_write(int fd, const void *buf, uint32_t count) {
    if (!buf) return -EFAULT;

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        const char *s = (const char *)buf;
        for (uint32_t i = 0; i < count; i++) console_put_char(s[i]);
        return (int)count;
    }
    if (fd == STDIN_FILENO) return -EBADF;

    open_file_t *f = fd_to_file(fd);
    if (!f) return -EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (count == 0) return 0;

    if (f->flags & O_APPEND) f->offset = f->node->length;

    /* The backend stores what fits (ramfs: VFS_MAX_FILE_SIZE per file;
     * disk: its own per-file limit and free space) and returns the short
     * count - a partial write. Only when nothing at all fits is it an
     * error. */
    ssize_t w = vfs_write(f->node, f->offset, count, (const uint8_t *)buf);
    if (w < 0) return -EIO;
    if (w == 0) return -ENOSPC;
    f->offset += (uint32_t)w;
    return (int)w;
}

int fileio_close(int fd) {
    process_t *p = process_current();
    if (!p) return -EBADF;
    if (fd >= 0 && fd < 3) return 0;   /* std streams: nothing to release */
    open_file_t *f = fd_to_file(fd);
    if (!f) return -EBADF;

    uint32_t irq = irq_save();
    if (--f->used <= 0) {
        f->used = 0;
        f->node = 0;
    }
    p->fd_table[fd] = 0;
    irq_restore(irq);
    return 0;
}

void fileio_fork_fds(process_t *child) {
    if (!child) return;
    for (int fd = 3; fd < PROCESS_MAX_FDS; fd++) {
        int slot = child->fd_table[fd];
        if (slot > 0 && slot <= FILEIO_MAX_OPEN && oft[slot - 1].used) {
            oft[slot - 1].used++;
        } else {
            child->fd_table[fd] = 0;
        }
    }
}

void fileio_close_all(process_t *p) {
    if (!p) return;
    for (int fd = 3; fd < PROCESS_MAX_FDS; fd++) {
        int slot = p->fd_table[fd];
        if (slot > 0 && slot <= FILEIO_MAX_OPEN && oft[slot - 1].used) {
            if (--oft[slot - 1].used <= 0) {
                oft[slot - 1].used = 0;
                oft[slot - 1].node = 0;
            }
        }
        p->fd_table[fd] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* errno text                                                           */
/* ------------------------------------------------------------------ */

typedef struct { int num; const char *name; const char *msg; } errinfo_t;

static const errinfo_t errtab[] = {
    { EPERM,   "EPERM",   "Operation not permitted" },
    { ENOENT,  "ENOENT",  "No such file or directory" },
    { EINTR,   "EINTR",   "Interrupted system call" },
    { EIO,     "EIO",     "Input/output error" },
    { EBADF,   "EBADF",   "Bad file descriptor" },
    { EACCES,  "EACCES",  "Permission denied" },
    { EFAULT,  "EFAULT",  "Bad address" },
    { EEXIST,  "EEXIST",  "File exists" },
    { ENOTDIR, "ENOTDIR", "Not a directory" },
    { EISDIR,  "EISDIR",  "Is a directory" },
    { EINVAL,  "EINVAL",  "Invalid argument" },
    { ENFILE,  "ENFILE",  "Too many open files in system" },
    { EMFILE,  "EMFILE",  "Too many open files" },
    { EFBIG,   "EFBIG",   "File too large" },
    { ENOSPC,  "ENOSPC",  "No space left on device" },
    { ENAMETOOLONG, "ENAMETOOLONG", "File name too long" },
};

const char *fileio_strerror(int err) {
    for (size_t i = 0; i < sizeof(errtab) / sizeof(errtab[0]); i++)
        if (errtab[i].num == err) return errtab[i].msg;
    return "Unknown error";
}

const char *fileio_errno_name(int err) {
    for (size_t i = 0; i < sizeof(errtab) / sizeof(errtab[0]); i++)
        if (errtab[i].num == err) return errtab[i].name;
    return "E?";
}

/* ------------------------------------------------------------------ */
/* Syscall tracer (strace)                                              */
/* ------------------------------------------------------------------ */

#define TRACE_STRSIZE 32     /* like strace's default "-s 32" */

typedef struct { char buf[256]; size_t len; } tline_t;

static void tl_putc(tline_t *t, char c) {
    if (t->len < sizeof(t->buf) - 1) t->buf[t->len++] = c;
    t->buf[t->len] = '\0';
}

static void tl_puts(tline_t *t, const char *s) {
    while (*s) tl_putc(t, *s++);
}

static void tl_num(tline_t *t, int v) {
    char tmp[12];
    itoa(v, tmp, 10);
    tl_puts(t, tmp);
}

static void tl_hex(tline_t *t, uint32_t v) {
    const char *d = "0123456789abcdef";
    char tmp[9];
    int i = 8;
    tmp[i] = '\0';
    do { tmp[--i] = d[v & 0xF]; v >>= 4; } while (v && i > 0);
    tl_puts(t, "0x");
    tl_puts(t, &tmp[i]);
}

static void tl_oct(tline_t *t, uint32_t v) {
    char tmp[12];
    int i = 11;
    tmp[i] = '\0';
    do { tmp[--i] = (char)('0' + (v & 7)); v >>= 3; } while (v && i > 0);
    tl_putc(t, '0');
    tl_puts(t, &tmp[i]);
}

/* Quoted, escaped data the way strace prints buffers. */
static void tl_data(tline_t *t, const uint8_t *p, uint32_t n) {
    uint32_t shown = n < TRACE_STRSIZE ? n : TRACE_STRSIZE;
    tl_putc(t, '"');
    for (uint32_t i = 0; i < shown; i++) {
        uint8_t c = p[i];
        switch (c) {
            case '\n': tl_puts(t, "\\n");  break;
            case '\t': tl_puts(t, "\\t");  break;
            case '\r': tl_puts(t, "\\r");  break;
            case '"':  tl_puts(t, "\\\""); break;
            case '\\': tl_puts(t, "\\\\"); break;
            default:
                if (c >= 32 && c < 127) {
                    tl_putc(t, (char)c);
                } else {
                    tl_putc(t, '\\');
                    tl_putc(t, (char)('0' + ((c >> 6) & 7)));
                    tl_putc(t, (char)('0' + ((c >> 3) & 7)));
                    tl_putc(t, (char)('0' + (c & 7)));
                }
        }
    }
    tl_putc(t, '"');
    if (n > shown) tl_puts(t, "...");
}

static void tl_result(tline_t *t, int ret) {
    tl_puts(t, " = ");
    if (ret >= 0) {
        tl_num(t, ret);
    } else {
        tl_puts(t, "-1 ");
        tl_puts(t, fileio_errno_name(-ret));
        tl_puts(t, " (");
        tl_puts(t, fileio_strerror(-ret));
        tl_puts(t, ")");
    }
}

static void trace_emit(const char *line) {
    process_t *p = process_current();
    vfs_node_t *out = p ? (vfs_node_t *)p->trace_out : 0;
    if (out) {
        uint32_t len = (uint32_t)string_length(line);
        uint32_t off = out->length;
        if (off + len + 1 <= VFS_MAX_FILE_SIZE) {
            vfs_write(out, off, len, (const uint8_t *)line);
            vfs_write(out, off + len, 1, (const uint8_t *)"\n");
        }
    } else {
        console_write(line);
        console_put_char('\n');
    }
}

static int tracing(uint32_t cls) {
    process_t *p = process_current();
    return p && (p->trace_mask & cls);
}

void fileio_trace_text(const char *line) {
    process_t *p = process_current();
    if (p && p->trace_mask) trace_emit(line);
}

void fileio_trace_open(const char *path, int flags, uint32_t mode, int ret) {
    if (!tracing(FILEIO_TRACE_OPEN)) return;
    tline_t t; t.len = 0; t.buf[0] = '\0';

    tl_puts(&t, "openat(AT_FDCWD, ");
    if (path) tl_data(&t, (const uint8_t *)path, (uint32_t)string_length(path));
    else      tl_puts(&t, "NULL");
    tl_puts(&t, ", ");

    int acc = flags & O_ACCMODE;
    tl_puts(&t, acc == O_WRONLY ? "O_WRONLY" : acc == O_RDWR ? "O_RDWR" : "O_RDONLY");
    if (flags & O_CREAT)  tl_puts(&t, "|O_CREAT");
    if (flags & O_EXCL)   tl_puts(&t, "|O_EXCL");
    if (flags & O_TRUNC)  tl_puts(&t, "|O_TRUNC");
    if (flags & O_APPEND) tl_puts(&t, "|O_APPEND");
    if (flags & O_CREAT) {
        tl_puts(&t, ", ");
        tl_oct(&t, mode & 07777);
    }
    tl_putc(&t, ')');
    tl_result(&t, ret);
    trace_emit(t.buf);
}

void fileio_trace_rw(int is_write, int fd, const void *buf,
                     uint32_t count, int ret) {
    if (!tracing(is_write ? FILEIO_TRACE_WRITE : FILEIO_TRACE_READ)) return;
    tline_t t; t.len = 0; t.buf[0] = '\0';

    tl_puts(&t, is_write ? "write(" : "read(");
    tl_num(&t, fd);
    tl_puts(&t, ", ");
    if (is_write) {
        tl_data(&t, (const uint8_t *)buf, count);
    } else if (ret >= 0) {
        tl_data(&t, (const uint8_t *)buf, (uint32_t)ret);
    } else {
        tl_hex(&t, (uint32_t)buf);
    }
    tl_puts(&t, ", ");
    tl_num(&t, (int)count);
    tl_putc(&t, ')');
    tl_result(&t, ret);
    trace_emit(t.buf);
}

void fileio_trace_close(int fd, int ret) {
    if (!tracing(FILEIO_TRACE_CLOSE)) return;
    tline_t t; t.len = 0; t.buf[0] = '\0';
    tl_puts(&t, "close(");
    tl_num(&t, fd);
    tl_putc(&t, ')');
    tl_result(&t, ret);
    trace_emit(t.buf);
}
