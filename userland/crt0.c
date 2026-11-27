/*
 * OrgitoOS - C runtime start-up for userland programs (crt0)
 *
 * kernel/exec.c starts a loaded ELF at its entry point, _start, in ring 3
 * with ESP at the top of the process's private user stack. _start calls
 * main() and passes its return value to the exit system call, so a
 * program can simply `return` from main like on Linux.
 *
 * Arguments (argc/argv) are not passed yet: main() is called as
 * main(void).
 */

#include "../include/usyscall.h"

int main(void);

void _start(void) {
    u_exit(main());
}
