/*
 * OrgitoOS - minimal C library for userland programs
 *
 * The non-inline half of include/ulibc.h: `errno`, strlen() and
 * strerror(). Linked into every userland ELF (see the Makefile). Runs in
 * ring 3, so it must not call anything in the kernel - the only way into
 * the kernel is int 0x80 (the inline wrappers in ulibc.h).
 */

#include "../include/ulibc.h"

int errno = 0;

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

const char *strerror(int errnum) {
    switch (errnum) {
        case 1:  return "Operation not permitted";
        case 2:  return "No such file or directory";
        case 4:  return "Interrupted system call";
        case 5:  return "Input/output error";
        case 8:  return "Exec format error";
        case 9:  return "Bad file descriptor";
        case 12: return "Cannot allocate memory";
        case 13: return "Permission denied";
        case 14: return "Bad address";
        case 17: return "File exists";
        case 20: return "Not a directory";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 23: return "Too many open files in system";
        case 24: return "Too many open files";
        case 27: return "File too large";
        case 28: return "No space left on device";
        case 36: return "File name too long";
        default: return "Unknown error";
    }
}
