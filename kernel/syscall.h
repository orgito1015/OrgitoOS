/*
 * OrgitoOS - System Call Interface (Phase 1)
 *
 * Ring 3 processes request kernel services via `int $0x80`:
 *   EAX = syscall number, EBX/ECX/EDX = arguments,
 *   EAX = return value.
 *
 * The register frame layout must stay in sync with the int80_handler
 * stub in arch/x86/syscall.S and fork_child_return in context.S.
 */

#ifndef ORGITOOS_KERNEL_SYSCALL_H
#define ORGITOOS_KERNEL_SYSCALL_H

#include <stdint.h>

/* Syscall numbers */
#define SYS_EXIT     0   /* exit(code)                   */
#define SYS_WRITE    1   /* write(str) -> chars written  */
#define SYS_GETPID   2   /* getpid()                     */
#define SYS_FORK     3   /* fork() -> child pid | 0      */
#define SYS_YIELD    4   /* yield()                      */
#define SYS_SLEEP    5   /* sleep(ms)                    */
#define SYS_GETPPID  6   /* getppid()                    */
#define SYS_WAIT     7   /* wait(&status) -> child pid   */
#define SYS_READ     8   /* read(buf, maxlen) -> chars read */
/* POSIX-style file I/O (kernel/fileio.c). Results are >= 0 on success or
 * -errno on failure; include/ulibc.h maps that to "-1 and errno". */
#define SYS_OPEN     9   /* open(path, flags, mode) -> fd      */
#define SYS_CLOSE    10  /* close(fd) -> 0                     */
#define SYS_FDREAD   11  /* read(fd, buf, count) -> bytes      */
#define SYS_FDWRITE  12  /* write(fd, buf, count) -> bytes     */
#define SYS_MAX      13

/*
 * Register frame pushed by int80_handler, lowest address first:
 * segment registers, then PUSHA block, then the CPU's interrupt frame.
 * useresp/ss are only present because int 0x80 always arrives from
 * ring 3 (the gate has DPL 3 and the kernel never uses it).
 */
typedef struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} regs_t;

/* Install the int 0x80 gate. Call after idt_init(). */
void syscall_init(void);

/* C-level dispatcher, called by the assembly stub. Writes the return
 * value into r->eax. */
void syscall_dispatch(regs_t *r);

#endif /* ORGITOOS_KERNEL_SYSCALL_H */
