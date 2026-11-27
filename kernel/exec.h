/*
 * OrgitoOS - exec(): load a real ELF32 binary into a new process.
 * See kernel/exec.c.
 */

#ifndef ORGITOOS_KERNEL_EXEC_H
#define ORGITOOS_KERNEL_EXEC_H

#include <stdint.h>
#include "../process/process.h"

/* Error codes reported through process_exec()'s `err` out-parameter
 * (same numbers as the matching Linux errno values). */
#define EXEC_ENOENT   2
#define EXEC_ENOEXEC  8
#define EXEC_ENOMEM  12
#define EXEC_EACCES  13
#define EXEC_EISDIR  21

/*
 * Load the ELF executable at `path` (absolute, or relative to the current
 * directory) into a NEW user process, child of the caller.
 *
 * The process is returned SUSPENDED: call process_start() to run it (the
 * caller can first adjust it, e.g. set keep_zombie before waiting for it).
 * Returns NULL on failure with *err set to an EXEC_* code. With `verbose`,
 * prints where the image was loaded.
 */
process_t *process_exec(const char *path, uint8_t priority, int verbose, int *err);

/* Message for an EXEC_* error code. */
const char *exec_strerror(int err);

/* Copy the programs embedded in the kernel image (built from userland/)
 * into /bin. Call once at boot, after vfs_init() and heap_init(). */
void exec_install_embedded_programs(void);

#endif /* ORGITOOS_KERNEL_EXEC_H */
