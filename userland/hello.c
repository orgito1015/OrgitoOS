/*
 * OrgitoOS - userland demo binary
 *
 * Unlike kernel/user_programs.c (kernel-linked C functions entered
 * directly by process_create_user()), this file is compiled and linked
 * completely separately (see the Makefile's userland rules and
 * userland/user.ld), producing a real, standalone ELF32 executable. It
 * is embedded into the kernel image as a byte blob and preloaded into
 * the VFS at /bin/hello (kernel/exec.c, exec_install_embedded_programs)
 * so `exec /bin/hello` has something genuine to load: kernel/exec.c
 * parses this file's ELF header and program headers and maps its
 * PT_LOAD segment at runtime, rather than jumping to a function pointer
 * that was already sitting in the kernel image.
 *
 * Freestanding, no libc: only raw int 0x80 syscalls via usyscall.h.
 */

#include "../include/usyscall.h"

static void u_itoa_unsigned(unsigned value, char *buf) {
    char tmp[12];
    int pos = 0;

    if (value == 0) {
        tmp[pos++] = '0';
    }
    while (value > 0) {
        tmp[pos++] = (char)('0' + (value % 10));
        value /= 10;
    }

    int i = 0;
    while (pos > 0) {
        buf[i++] = tmp[--pos];
    }
    buf[i] = '\0';
}

void _start(void) {
    char pid_buf[12];

    u_write("[hello] real ELF32 binary running in ring 3, loaded via exec()!\n");

    u_itoa_unsigned((unsigned)u_getpid(), pid_buf);
    u_write("[hello] my pid is ");
    u_write(pid_buf);
    u_write("\n");

    u_write("[hello] exiting via sys_exit\n");
    u_exit(0);
}
