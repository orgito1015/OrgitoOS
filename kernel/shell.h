/*
 * OrgitoOS - Shell Interface
 * 
 * Provides command-line shell functionality for the kernel.
 */

#ifndef ORGITOOS_KERNEL_SHELL_H
#define ORGITOOS_KERNEL_SHELL_H

#include <stddef.h>

/* Maximum number of arguments a command can have */
#define SHELL_MAX_ARGS 16

/* Maximum number of stages in a `cmd1 | cmd2 | ...` pipeline */
#define SHELL_MAX_PIPE_STAGES 8

/* Command handler function pointer type */
typedef void (*command_handler_t)(int argc, char** argv);

/* Command structure */
typedef struct {
    const char* name;
    const char* description;
    command_handler_t handler;
} shell_command_t;

/* Initialize the shell */
void shell_init(void);

/* Execute a command string */
void shell_execute(char* input);

/* Register a command */
void shell_register_command(const char* name, const char* description, command_handler_t handler);

/*
 * Pipe stdin: when a command is running as a non-first stage of a
 * `cmd1 | cmd2` pipeline, its "stdin" is the captured stdout of the
 * previous stage rather than the keyboard. Commands that want to
 * participate as a pipe filter (e.g. grep, wc) call these instead of
 * reading the keyboard directly.
 */
int shell_stdin_available(void);
int shell_stdin_read_line(char* buf, int max);

#endif /* ORGITOOS_KERNEL_SHELL_H */
