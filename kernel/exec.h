/*
 * OrgitoOS - exec(): load a real ELF32 binary from the VFS into a new
 * process's own address space (see kernel/exec.c for the full design
 * note on why this is spawn-style rather than in-place image
 * replacement).
 */

#ifndef ORGITOOS_KERNEL_EXEC_H
#define ORGITOOS_KERNEL_EXEC_H

#include <stdint.h>
#include "../process/process.h"

/*
 * Load the ELF32 executable at `path` (an absolute VFS path) and spawn it
 * as a new ring 3 process at the given scheduler priority. Returns the new
 * process on success, or NULL (with a diagnostic on the console) if the
 * file is missing, not a valid statically-linked 32-bit x86 ELF
 * executable, or a page/PCB allocation fails partway through.
 */
process_t *process_exec(const char *path, uint8_t priority);

/* Preload the kernel-embedded demo binary (built from userland/hello.c,
 * see the Makefile's userland rules) into the VFS at /bin/hello so
 * `exec /bin/hello` has a real file to load. Call once at boot, after
 * vfs_init(). */
void exec_install_embedded_programs(void);

#endif /* ORGITOOS_KERNEL_EXEC_H */
