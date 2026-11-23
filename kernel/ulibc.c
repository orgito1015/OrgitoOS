/*
 * OrgitoOS - runtime support for include/ulibc.h
 *
 * The parts of the tiny user C library that cannot be header-only:
 * the global `errno` and strerror(). (strlen() comes from string.c.)
 */

#include "fileio.h"

int errno = 0;

const char *strerror(int errnum) {
    return fileio_strerror(errnum);
}
