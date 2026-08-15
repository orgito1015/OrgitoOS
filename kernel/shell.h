/*
 * OrgitoOS - Shell Interface
 * 
 * Provides command-line shell functionality for the kernel.
 */

#ifndef ORGITOOS_KERNEL_SHELL_H
#define ORGITOOS_KERNEL_SHELL_H

/* Maximum number of arguments a command can have */
#define SHELL_MAX_ARGS 16

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

#endif /* ORGITOOS_KERNEL_SHELL_H */
