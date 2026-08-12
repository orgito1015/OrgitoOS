/*
 * OrgitoOS - Shell Implementation
 */

#include "shell.h"
#include "string.h"
#include "commands.h"
#include "../drivers/console.h"

/* Maximum number of registered commands */
#define MAX_COMMANDS 48

/* Command registry */
static shell_command_t command_table[MAX_COMMANDS];
static int command_count = 0;

/*
 * Pipe stdin state (see shell_stdin_available/shell_stdin_read_line in
 * shell.h). Set by the pipeline runner before invoking a non-first stage's
 * handler, cleared right after. Not reentrant - fine, since pipeline
 * stages execute strictly sequentially in this single-threaded shell.
 */
static const char* pipe_stdin_data = 0;
static size_t pipe_stdin_len = 0;
static size_t pipe_stdin_pos = 0;

int shell_stdin_available(void) {
    return pipe_stdin_data != 0;
}

int shell_stdin_read_line(char* buf, int max) {
    if (!pipe_stdin_data || pipe_stdin_pos >= pipe_stdin_len || max <= 0) {
        return 0;
    }
    int n = 0;
    while (pipe_stdin_pos < pipe_stdin_len && n < max - 1) {
        char c = pipe_stdin_data[pipe_stdin_pos++];
        if (c == '\n') {
            break;
        }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return 1;
}

static void set_pipe_stdin(const char* data, size_t len) {
    pipe_stdin_data = data;
    pipe_stdin_len = len;
    pipe_stdin_pos = 0;
}

static void clear_pipe_stdin(void) {
    pipe_stdin_data = 0;
    pipe_stdin_len = 0;
    pipe_stdin_pos = 0;
}

/*
 * Initialize the shell
 * Registers all built-in commands
 */
void shell_init(void) {
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

/*
 * Parse and run a single (non-piped) command line: look it up in the
 * registry and invoke its handler, or print "command not found".
 */
static void run_command_line(char* line) {
    if (line == 0 || *line == '\0') {
        return;
    }

    char* argv[SHELL_MAX_ARGS];
    int argc = parse_command(line, argv, SHELL_MAX_ARGS);

    if (argc == 0) {
        return;
    }

    const char* cmd_name = argv[0];
    for (int i = 0; i < command_count; i++) {
        if (string_compare(cmd_name, command_table[i].name) == 0) {
            command_table[i].handler(argc, argv);
            return;
        }
    }

    console_write("Command not found: ");
    console_write(cmd_name);
    console_write("\n");
    console_write("Type 'help' for a list of available commands.\n");
}

/*
 * Split `input` in place on top-level '|' characters into up to
 * max_stages trimmed substrings. Returns the number of stages found
 * (always >= 1 for non-empty input). No quoting support - this is a
 * simple shell and '|' always separates pipeline stages.
 */
static int split_pipeline(char* input, char* stages[], int max_stages) {
    int n = 0;
    char* p = input;

    stages[n++] = p;
    while (*p != '\0') {
        if (*p == '|') {
            *p = '\0';
            p++;
            if (n < max_stages) {
                stages[n++] = p;
            }
        } else {
            p++;
        }
    }

    /* Trim leading/trailing whitespace from each stage */
    for (int i = 0; i < n; i++) {
        char* s = stages[i];
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        stages[i] = s;

        char* e = s + string_length(s);
        while (e > s && is_whitespace(e[-1])) {
            *(--e) = '\0';
        }
    }

    return n;
}

/* Capture buffer size for one pipeline stage's stdout. Static (not on the
 * stack) since two of these live for the duration of shell_execute(). */
#define PIPE_CAPTURE_SIZE 8192

/*
 * Execute a command string, with support for `cmd1 | cmd2 | ...` pipelines:
 * each non-final stage's console output is captured (console_set_capture)
 * instead of drawn to the screen, then handed to the next stage as its
 * "stdin" (shell_stdin_available/shell_stdin_read_line) - real data flowing
 * between commands, not just a printed "would pipe" message.
 */
void shell_execute(char* input) {
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

    char* stages[SHELL_MAX_PIPE_STAGES];
    int nstages = split_pipeline(input, stages, SHELL_MAX_PIPE_STAGES);

    if (nstages <= 1) {
        clear_pipe_stdin();
        run_command_line(stages[0]);
        return;
    }

    static char capture_a[PIPE_CAPTURE_SIZE];
    static char capture_b[PIPE_CAPTURE_SIZE];
    char* next_out = capture_a;
    const char* stdin_data = 0;
    size_t stdin_len = 0;

    for (int i = 0; i < nstages; i++) {
        int is_last = (i == nstages - 1);

        if (stdin_data != 0) {
            set_pipe_stdin(stdin_data, stdin_len);
        } else {
            clear_pipe_stdin();
        }

        if (is_last) {
            run_command_line(stages[i]);
            clear_pipe_stdin();
        } else {
            console_set_capture(next_out, PIPE_CAPTURE_SIZE);
            run_command_line(stages[i]);
            size_t len = console_end_capture();
            clear_pipe_stdin();

            stdin_data = next_out;
            stdin_len = len;
            next_out = (next_out == capture_a) ? capture_b : capture_a;
        }
    }
}

/*
 * Get all registered commands (used by help command)
 */
const shell_command_t* shell_get_commands(int* count) {
    *count = command_count;
    return command_table;
}
