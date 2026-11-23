/*
 * OrgitoOS - Shell Implementation
 */

#include "shell.h"
#include "string.h"
#include "commands.h"
#include "../drivers/console.h"

/* Maximum number of registered commands */
#define MAX_COMMANDS 48

/* Command registry - one shared table for every shell instance/pane */
static shell_command_t command_table[MAX_COMMANDS];
static int command_count = 0;
static int shell_initialized = 0;

/*
 * Initialize the shell: registers all built-in commands.
 *
 * Idempotent by design: with two split-screen shell instances
 * (kernel/kernel.c launches one per pane), this must run exactly once
 * against the single shared command_table - calling it twice would either
 * race (two threads concurrently resetting/repopulating the same table)
 * or just double-register everything. kmain() already calls this once
 * before creating either shell process; this guard is the defensive
 * second layer in case anything else ever calls it again.
 */
void shell_init(void) {
    if (shell_initialized) {
        return;
    }
    shell_initialized = 1;

    command_count = 0;

    /* Register built-in commands */
    commands_register_all();
}

/*
 * Register a command
 */
void shell_register_command(const char* name, const char* description, command_handler_t handler) {
    if (command_count >= MAX_COMMANDS) {
        console_write("Error: Maximum number of commands reached\n");
        return;
    }
    
    command_table[command_count].name = name;
    command_table[command_count].description = description;
    command_table[command_count].handler = handler;
    command_count++;
}

/*
 * Parse command line input into argc/argv
 * Returns argc (number of arguments)
 */
static int parse_command(char* input, char** argv, int max_args) {
    int argc = 0;
    char* token = string_tokenize(input, " \t\n\r");
    
    while (token != 0 && argc < max_args) {
        argv[argc] = token;
        argc++;
        token = string_tokenize(0, " \t\n\r");
    }
    
    return argc;
}

/* Exit status of the most recent command (see shell.h) */
static int last_status = 0;

void shell_set_status(int status) { last_status = status; }
int  shell_get_status(void)       { return last_status; }

const shell_command_t* shell_find_command(const char* name) {
    if (name[0] == '.' && name[1] == '/') {
        name += 2;     /* "./FileCopy" -> "FileCopy" */
    }
    for (int i = 0; i < command_count; i++) {
        if (string_compare(name, command_table[i].name) == 0) {
            return &command_table[i];
        }
    }
    return 0;
}

static void shell_execute_one(char* input);

/*
 * Execute a command line. Supports "cmd1 && cmd2 && ...": each command
 * runs only if the previous one exited with status 0.
 */
void shell_execute(char* input) {
    if (input == 0) {
        return;
    }
    char* seg = input;
    for (;;) {
        char* amp = 0;
        for (char* p = seg; *p; p++) {
            if (p[0] == '&' && p[1] == '&') { amp = p; break; }
        }
        if (amp) {
            amp[0] = '\0';
        }
        shell_execute_one(seg);
        if (!amp || last_status != 0) {
            return;
        }
        seg = amp + 2;
    }
}

/*
 * Execute a single command (no && chaining)
 */
static void shell_execute_one(char* input) {
    last_status = 0;
    /* Skip empty input */
    if (input == 0 || *input == '\0') {
        return;
    }
    
    /* Check if input is only whitespace */
    int only_whitespace = 1;
    for (char* p = input; *p != '\0'; p++) {
        if (!is_whitespace(*p)) {
            only_whitespace = 0;
            break;
        }
    }
    
    if (only_whitespace) {
        return;
    }
    
    /* Parse the command into arguments */
    char* argv[SHELL_MAX_ARGS];
    int argc = parse_command(input, argv, SHELL_MAX_ARGS);
    
    if (argc == 0) {
        return;
    }
    
    /* Look up the command in the command table */
    const char* cmd_name = argv[0];
    const shell_command_t* cmd = shell_find_command(cmd_name);
    if (cmd) {
        cmd->handler(argc, argv);
        return;
    }

    /* Command not found */
    last_status = 127;
    console_write("Command not found: ");
    console_write(cmd_name);
    console_write("\n");
    console_write("Type 'help' for a list of available commands.\n");
}

/*
 * Get all registered commands (used by help command)
 */
const shell_command_t* shell_get_commands(int* count) {
    *count = command_count;
    return command_table;
}
