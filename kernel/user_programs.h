/*
 * OrgitoOS - Userland Demo Programs (Phase 1)
 *
 * Small ring 3 programs compiled into the kernel image and launched
 * via process_create_user(). They interact with the kernel exclusively
 * through int 0x80 syscalls (see include/usyscall.h).
 */

#ifndef ORGITOOS_KERNEL_USER_PROGRAMS_H
#define ORGITOOS_KERNEL_USER_PROGRAMS_H

/* Prints greetings via sys_write, sleeps, and exits. */
void uprog_hello(void);

/* fork() demo: parent forks a child, both print, parent wait()s. */
void uprog_forktest(void);

/* Counter that yields between iterations (scheduler demo). */
void uprog_counter(void);

/* Reads a line via sys_read and echoes it back (read() demo). */
void uprog_echo(void);

#endif /* ORGITOOS_KERNEL_USER_PROGRAMS_H */
