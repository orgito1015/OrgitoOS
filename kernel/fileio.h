/*
 * OrgitoOS - POSIX-style file I/O system calls
 *
 * open()/read()/write()/close() over the VFS, with per-process file
 * descriptor tables and Linux-compatible flag values and errno numbers.
 * Also hosts the in-kernel syscall tracer used by the `strace` shell
 * command. See docs/assignments/detyre1/ for the course assignment that
 * motivated this layer.
 */

#ifndef ORGITOOS_KERNEL_FILEIO_H
#define ORGITOOS_KERNEL_FILEIO_H

#include <stdint.h>

/* open() flags - same numeric values as Linux/i386 */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

/* errno values - same numbers as Linux */
#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define EBADF    9
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENFILE  23
#define EMFILE  24
#define EFBIG   27
#define ENOSPC  28
#define ENAMETOOLONG 36

/* Standard descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Permission bits stored in vfs_node_t.flags. A fresh node has flags == 0,
 * which means "default permissions" (rw-r--r--, i.e. 0644). Once chmod has
 * been used, VFS_FLAG_MODE_SET is set and the low 9 bits hold the mode. */
#define VFS_FLAG_MODE_SET 0x80000000u
#define VFS_MODE_MASK     0x1FFu
#define VFS_DEFAULT_MODE  0644u

/* Trace classes for process_t.trace_mask */
#define FILEIO_TRACE_OPEN   0x1
#define FILEIO_TRACE_READ   0x2
#define FILEIO_TRACE_WRITE  0x4
#define FILEIO_TRACE_CLOSE  0x8
#define FILEIO_TRACE_ALL    0xF

struct process;
struct vfs_node;

/* Kernel-side implementations. Return >= 0 on success, -errno on error. */
int fileio_open(const char *path, int flags, uint32_t mode);
int fileio_read(int fd, void *buf, uint32_t count);
int fileio_write(int fd, const void *buf, uint32_t count);
int fileio_close(int fd);

/* Close every descriptor >= 3 owned by `p` (process exit / program end). */
void fileio_close_all(struct process *p);

/* Effective permission bits of a node (0644 unless chmod'ed). */
uint32_t fileio_node_mode(const struct vfs_node *node);

/* Resolve a path relative to the current working directory. */
struct vfs_node *fileio_lookup(const char *path);

/* Human-readable message for an errno value (Linux wording). */
const char *fileio_strerror(int err);

/* Symbolic name of an errno value ("ENOENT"). */
const char *fileio_errno_name(int err);

/* Syscall tracing (`strace`). Called by the syscall dispatcher after each
 * file syscall completes, with the raw arguments and the result. */
void fileio_trace_open(const char *path, int flags, uint32_t mode, int ret);
void fileio_trace_rw(int is_write, int fd, const void *buf,
                     uint32_t count, int ret);
void fileio_trace_close(int fd, int ret);
void fileio_trace_text(const char *line);

#endif /* ORGITOOS_KERNEL_FILEIO_H */
