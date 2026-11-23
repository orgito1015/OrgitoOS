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

/* Exit status of the last command (0 = success). Commands that can fail
 * report it with shell_set_status(); "cmd1 && cmd2" runs cmd2 only when
 * cmd1 finished with status 0. Unknown commands give status 127. */
void shell_set_status(int status);
int  shell_get_status(void);

/* Look up a registered command by name ("./name" is accepted too). */
const shell_command_t* shell_find_command(const char* name);

/* Get all registered commands (used by help) */
const shell_command_t* shell_get_commands(int* count);

#endif /* ORGITOOS_KERNEL_SHELL_H */
