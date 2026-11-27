/*
 * OrgitoOS - minimal POSIX-like user library
 *
 * Lets ordinary Unix-style C code (open/read/write/close, errno,
 * strerror) build for OrgitoOS unchanged. Every function here enters the
 * kernel through the `int $0x80` gate - the same path ring 3 code uses -
 * and is therefore visible to the `strace` shell command.
 *
 * Kernel return convention: >= 0 success, -errno failure. These wrappers
 * convert it to the POSIX convention: return -1 and set `errno`.
 *
 * Programs that include this header are compiled with -D__ORGITOOS__
 * (see the Makefile), so the same source file can also be compiled on
 * Linux, where it includes the real <fcntl.h>/<unistd.h> instead.
 */

#ifndef ORGITOOS_INCLUDE_ULIBC_H
#define ORGITOOS_INCLUDE_ULIBC_H

#include <stddef.h>
#include <stdint.h>
#include "usyscall.h"

#ifndef O_RDONLY
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#endif

#ifndef ENOENT
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define EBADF    9
#define EACCES  13
#define EEXIST  17
#define EISDIR  21
#define EINVAL  22
#define ENOSPC  28
#endif

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

typedef int32_t ssize_t_u;
typedef uint32_t mode_t_u;

/* errno lives in kernel/ulibc.c (one per system: programs run one at a
 * time from the shell). */
extern int errno;

/* String helpers, provided by kernel/string.c and kernel/ulibc.c */
size_t strlen(const char *s);
const char *strerror(int errnum);

static inline int _syscall3(int num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(num), "b"(a1), "c"(a2), "d"(a3)
                         : "memory");
    return ret;
}

static inline int __ulibc_ret(int r) {
    if (r < 0) {
        errno = -r;
        return -1;
    }
    return r;
}

static inline int open(const char *path, int flags, ...) {
    /* The mode argument is only meaningful with O_CREAT; read it through
     * a fixed third parameter slot via the builtin varargs machinery. */
    __builtin_va_list ap;
    uint32_t mode = 0;
    __builtin_va_start(ap, flags);
    if (flags & O_CREAT) mode = __builtin_va_arg(ap, uint32_t);
    __builtin_va_end(ap);
    return __ulibc_ret(_syscall3(SYS_OPEN, (uint32_t)path, (uint32_t)flags, mode));
}

static inline ssize_t_u read(int fd, void *buf, size_t count) {
    return __ulibc_ret(_syscall3(SYS_FDREAD, (uint32_t)fd, (uint32_t)buf, (uint32_t)count));
}

static inline ssize_t_u write(int fd, const void *buf, size_t count) {
    return __ulibc_ret(_syscall3(SYS_FDWRITE, (uint32_t)fd, (uint32_t)buf, (uint32_t)count));
}

static inline int close(int fd) {
    return __ulibc_ret(_syscall1(SYS_CLOSE, (uint32_t)fd));
}

#define ssize_t ssize_t_u
#define mode_t  mode_t_u

#endif /* ORGITOOS_INCLUDE_ULIBC_H */
