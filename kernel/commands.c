/*
 * OrgitoOS - Built-in Shell Commands Implementation
 */

#include "commands.h"
#include "shell.h"
#include "string.h"
#include "kernel.h"
#include "../drivers/console.h"
#include "../drivers/timer.h"
#include "../arch/x86/ports.h"
#include "../fs/vfs.h"
#include "../memory/pmm.h"
#include "../memory/heap.h"
#include "../include/ipc.h"
#include "../include/smp.h"
#include "../include/gui.h"
#include "../include/network.h"
#include "../include/script.h"

/* Forward declaration for accessing command table */
const shell_command_t* shell_get_commands(int* count);

/*
 * Helper function to build absolute path from relative path
 * Returns 0 on success, -1 on error
 */
static int build_absolute_path(const char* relative_path, char* abs_path, size_t abs_path_size) {
    (void)abs_path_size;  /* Unused - reserved for future bounds checking */
    
    vfs_node_t* current = kernel_get_current_directory();
    
    /* Build current path first */
    char current_path[VFS_MAX_PATH_LENGTH];
    current_path[0] = '\0';
    
    if (current && current->parent != current) {
        vfs_node_t* node = current;
        char temp[VFS_MAX_PATH_LENGTH];
        while (node && node->parent != node) {
            string_copy(temp, "/");
            string_concat(temp, node->name);
            string_concat(temp, current_path);
            string_copy(current_path, temp);
            node = node->parent;
        }
    }
    
    if (current_path[0] == '\0') {
        string_copy(abs_path, "/");
    } else {
        string_copy(abs_path, current_path);
    }
    
    string_concat(abs_path, "/");
    string_concat(abs_path, relative_path);
    
    return 0;
}

/*
 * Register all built-in commands
 */
void commands_register_all(void) {
    shell_register_command("help", "Display list of available commands", cmd_help);
    shell_register_command("clear", "Clear the console screen", cmd_clear);
    shell_register_command("echo", "Print text to console", cmd_echo);
    shell_register_command("uname", "Display OS name and version", cmd_uname);
    shell_register_command("uptime", "Show system uptime", cmd_uptime);
    shell_register_command("pwd", "Print current working directory", cmd_pwd);
    shell_register_command("ls", "List directory contents [-a] [-l] [-R]", cmd_ls);
    shell_register_command("cd", "Change directory", cmd_cd);
    shell_register_command("cat", "Display file contents [-n] [file...]", cmd_cat);
    shell_register_command("stat", "Show file or directory metadata", cmd_stat);
    shell_register_command("mkdir", "Create a directory", cmd_mkdir);
    shell_register_command("touch", "Create an empty file", cmd_touch);
    shell_register_command("write", "Write text to a file [write <file> <text...>]", cmd_write);
    shell_register_command("rm", "Remove a file or directory", cmd_rm);
    shell_register_command("grep", "Filter lines matching a pattern [grep <pattern> [file]] (pipeable)", cmd_grep);
    shell_register_command("wc", "Count lines/words/bytes [wc [file]] (pipeable)", cmd_wc);
    shell_register_command("meminfo", "Show physical and heap memory usage", cmd_meminfo);
    shell_register_command("ping", "Ping an IPv4 host over the real NIC [ping <ip>]", cmd_ping);
    shell_register_command("tcpconnect", "Connect over real TCP [tcpconnect <ip> <port> [message]]", cmd_tcpconnect);
    shell_register_command("reboot", "Reboot the system", cmd_reboot);
    
    /* New feature test commands */
    shell_register_command("test_ipc", "Test IPC mechanisms (pipes, message queues)", cmd_test_ipc);
    shell_register_command("test_smp", "Test multi-core SMP support", cmd_test_smp);
    shell_register_command("test_gui", "Test GUI/windowing system", cmd_test_gui);
    shell_register_command("test_net", "Test networking stack", cmd_test_net);
    shell_register_command("test_script", "Test shell scripting", cmd_test_script);

    /* Phase 1: process management */
    shell_register_command("ps", "List processes", cmd_ps);
    shell_register_command("kill", "Terminate a process [kill <pid>]", cmd_kill);
    shell_register_command("spawn", "Spawn demo kernel threads [spawn <n>]", cmd_spawn);
    shell_register_command("forktest", "Run ring 3 fork()/wait() demo", cmd_forktest);
    shell_register_command("user", "Run ring 3 hello program (syscalls)", cmd_user);
    shell_register_command("counters", "Run N ring 3 counters [counters <n>]", cmd_counters);
    shell_register_command("uecho", "Run ring 3 read() demo (type a line back)", cmd_uecho);
    shell_register_command("psleep", "Sleep the shell [psleep <ms>]", cmd_psleep);
    shell_register_command("sched", "Show scheduler statistics", cmd_sched);
    shell_register_command("exec", "Load and run a real ELF32 binary [exec <path>]", cmd_exec);
}

/*
 * Help command - Display list of available commands
 */
void cmd_help(int argc, char** argv) {
    (void)argc;  /* Unused parameter */
    (void)argv;  /* Unused parameter */
    
    console_write("\nAvailable Commands:\n");
    console_write("===================\n\n");
    
    int count;
    const shell_command_t* commands = shell_get_commands(&count);
    
    for (int i = 0; i < count; i++) {
        console_write("  ");
        console_write(commands[i].name);
        
        /* Add padding */
        size_t name_len = string_length(commands[i].name);
        for (size_t j = name_len; j < 12; j++) {
            console_write(" ");
        }
        
        console_write("- ");
        console_write(commands[i].description);
        console_write("\n");
    }
    
    console_write("\n");
}

/*
 * Clear command - Clear the console screen
 */
void cmd_clear(int argc, char** argv) {
    (void)argc;  /* Unused parameter */
    (void)argv;  /* Unused parameter */
    
    console_clear();
}

/*
 * Echo command - Print text to console
 */
void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            console_write(" ");
        }
        console_write(argv[i]);
    }
    console_write("\n");
}

/*
 * Uname command - Display OS name and version
 */
void cmd_uname(int argc, char** argv) {
    (void)argc;  /* Unused parameter */
    (void)argv;  /* Unused parameter */
    
    console_write("OrgitoOS version 0.1.0\n");
    console_write("Architecture: i386 (32-bit x86)\n");
    console_write("Kernel: Educational Operating System\n");
}

/*
 * Helper function to print a decimal number
 */
static void print_number(uint32_t value) {
    if (value == 0) {
        console_put_char('0');
        return;
    }
    
    char buffer[11];  /* Max 10 digits + null terminator */
    int i = 10;
    buffer[i] = '\0';
    
    while (value > 0 && i > 0) {
        i--;
        buffer[i] = '0' + (value % 10);
        value /= 10;
    }
    
    console_write(&buffer[i]);
}

/*
 * Uptime command - Show system uptime
 */
void cmd_uptime(int argc, char** argv) {
    (void)argc;  /* Unused parameter */
    (void)argv;  /* Unused parameter */
    
    uint64_t uptime_ms = timer_get_uptime_ms();
    
    /*
     * Use 32-bit arithmetic to avoid 64-bit division library dependency.
     * NOTE: This will wrap after ~49.7 days (2^32 milliseconds).
     * This is acceptable for an educational OS but should be addressed
     * if the system needs to track longer uptimes.
     */
    uint32_t uptime_ms_low = (uint32_t)uptime_ms;
    
    /* Calculate time components */
    uint32_t total_seconds = uptime_ms_low / 1000;
    uint32_t milliseconds = uptime_ms_low % 1000;
    uint32_t seconds = total_seconds % 60;
    uint32_t minutes = (total_seconds / 60) % 60;
    uint32_t hours = (total_seconds / 3600) % 24;
    uint32_t days = total_seconds / 86400;
    
    console_write("System uptime: ");
    
    if (days > 0) {
        print_number(days);
        console_write(" day");
        if (days > 1) console_write("s");
        console_write(", ");
    }
    
    if (hours > 0 || days > 0) {
        print_number(hours);
        console_write(" hour");
        if (hours != 1) console_write("s");
        console_write(", ");
    }
    
    if (minutes > 0 || hours > 0 || days > 0) {
        print_number(minutes);
        console_write(" minute");
        if (minutes != 1) console_write("s");
        console_write(", ");
    }
    
    print_number(seconds);
    console_write(".");
    
    /* Print milliseconds with leading zeros */
    if (milliseconds < 100) console_put_char('0');
    if (milliseconds < 10) console_put_char('0');
    print_number(milliseconds);
    
    console_write(" seconds\n");
}

/*
 * PWD command - Print current working directory
 */
void cmd_pwd(int argc, char** argv) {
    (void)argc;  /* Unused parameter */
    (void)argv;  /* Unused parameter */
    
    vfs_node_t* current = kernel_get_current_directory();
    if (!current) {
        console_write("/\n");
        return;
    }
    
    /* Build path by walking up to root */
    char path[VFS_MAX_PATH_LENGTH];
    char temp[VFS_MAX_PATH_LENGTH];
    path[0] = '\0';
    
    vfs_node_t* node = current;
    while (node && node->parent != node) {  /* Stop at root (root is its own parent) */
        /* Prepend current node name */
        string_copy(temp, "/");
        string_concat(temp, node->name);
        string_concat(temp, path);
        string_copy(path, temp);
        
        node = node->parent;
    }
    
    /* If path is empty, we're at root */
    if (path[0] == '\0') {
        console_write("/\n");
    } else {
        console_write(path);
        console_write("\n");
    }
}

/*
 * Helper: print a uint32_t padded to a fixed width (right-aligned, space-padded)
 */
static void print_number_padded(uint32_t value, int width) {
    /* 11 bytes: 10 digits max for uint32_t (4294967295) + null terminator */
    char buf[11];
    int i = 10;
    buf[i] = '\0';
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value > 0 && i > 0) {
            buf[--i] = '0' + (value % 10);
            value /= 10;
        }
    }
    /* Calculate number of digits */
    int digits = 10 - i;
    for (int j = digits; j < width; j++) {
        console_put_char(' ');
    }
    console_write(&buf[i]);
}

/*
 * Helper: list a single directory, used by cmd_ls for default and -R mode.
 * flag_long: show long format; flag_all: show dotfiles; print_header: print path header for -R
 */
static void ls_list_dir(vfs_node_t* dir, int flag_long, int flag_all,
                        int print_header, const char* header_path) {
    if (print_header) {
        console_write(header_path);
        console_write(":\n");
    }

    for (uint32_t i = 0; i < dir->child_count; i++) {
        vfs_node_t* child = dir->children[i];
        if (!child) continue;

        /* Skip dotfiles unless -a is set */
        if (!flag_all && child->name[0] == '.') continue;

        if (flag_long) {
            /* Long format: type size name */
            console_put_char(child->type == NODE_DIRECTORY ? 'd' : '-');
            console_write("  ");
            print_number_padded(child->length, 8);
            console_write("  ");
            console_write(child->name);
            if (child->type == NODE_DIRECTORY) {
                console_put_char('/');
            }
            console_put_char('\n');
        } else {
            console_write(child->name);
            if (child->type == NODE_DIRECTORY) {
                console_put_char('/');
            }
            console_put_char(' ');
        }
    }

    if (!flag_long) {
        console_put_char('\n');
    }
}

/*
 * Helper: recursively list directories for -R flag.
 * path_buf must be at least VFS_MAX_PATH_LENGTH bytes.
 */
static void ls_recursive(vfs_node_t* dir, int flag_long, int flag_all,
                         char* path_buf) {
    ls_list_dir(dir, flag_long, flag_all, 1, path_buf);

    /* Recurse into subdirectories */
    for (uint32_t i = 0; i < dir->child_count; i++) {
        vfs_node_t* child = dir->children[i];
        if (!child || child->type != NODE_DIRECTORY) continue;
        if (!flag_all && child->name[0] == '.') continue;

        /* Build child path */
        char child_path[VFS_MAX_PATH_LENGTH];
        string_copy(child_path, path_buf);
        /* Avoid double slash at root */
        if (child_path[0] == '/' && child_path[1] == '\0') {
            string_concat(child_path, child->name);
        } else {
            string_concat(child_path, "/");
            string_concat(child_path, child->name);
        }
        ls_recursive(child, flag_long, flag_all, child_path);
    }
}

/*
 * LS command - List directory contents
 * Usage: ls [-a] [-l] [-R] [-h|--help] [path]
 */
void cmd_ls(int argc, char** argv) {
    int flag_all = 0;
    int flag_long = 0;
    int flag_recursive = 0;
    const char* path_arg = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (string_compare(argv[i], "--help") == 0 || string_compare(argv[i], "-h") == 0) {
            console_write("Usage: ls [-a] [-l] [-R] [-h] [path]\n");
            console_write("  -a    include entries starting with '.'\n");
            console_write("  -l    long listing format (type, size, name)\n");
            console_write("  -R    list subdirectories recursively\n");
            console_write("  -h    show this help message\n");
            return;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            /* Flag argument: parse each character */
            for (int j = 1; argv[i][j] != '\0'; j++) {
                switch (argv[i][j]) {
                    case 'a': flag_all = 1; break;
                    case 'l': flag_long = 1; break;
                    case 'R': flag_recursive = 1; break;
                    default:
                        console_write("ls: invalid option -- '");
                        console_put_char(argv[i][j]);
                        console_write("'\nTry 'ls -h' for more information.\n");
                        return;
                }
            }
        } else {
            path_arg = argv[i];
        }
    }

    vfs_node_t* dir;

    if (!path_arg) {
        dir = kernel_get_current_directory();
    } else if (path_arg[0] == '/') {
        dir = vfs_resolve_path(path_arg);
    } else {
        char abs_path[VFS_MAX_PATH_LENGTH];
        build_absolute_path(path_arg, abs_path, VFS_MAX_PATH_LENGTH);
        dir = vfs_resolve_path(abs_path);
    }

    if (!dir) {
        console_write("ls: cannot access '");
        console_write(path_arg ? path_arg : ".");
        console_write("': No such file or directory\n");
        return;
    }

    if (dir->type != NODE_DIRECTORY) {
        /* Single file: just print its info */
        if (flag_long) {
            console_put_char('-');
            console_write("  ");
            print_number_padded(dir->length, 8);
            console_write("  ");
            console_write(dir->name);
            console_put_char('\n');
        } else {
            console_write(dir->name);
            console_put_char('\n');
        }
        return;
    }

    if (flag_recursive) {
        /* Build starting path string */
        char start_path[VFS_MAX_PATH_LENGTH];
        if (path_arg) {
            string_copy(start_path, path_arg);
        } else {
            /* Use current directory path */
            vfs_node_t* cur = dir;
            start_path[0] = '\0';
            if (cur->parent == cur) {
                string_copy(start_path, "/");
            } else {
                char temp[VFS_MAX_PATH_LENGTH];
                while (cur && cur->parent != cur) {
                    string_copy(temp, "/");
                    string_concat(temp, cur->name);
                    string_concat(temp, start_path);
                    string_copy(start_path, temp);
                    cur = cur->parent;
                }
                if (start_path[0] == '\0') {
                    string_copy(start_path, "/");
                }
            }
        }
        ls_recursive(dir, flag_long, flag_all, start_path);
    } else {
        ls_list_dir(dir, flag_long, flag_all, 0, 0);
    }
}

/*
 * CD command - Change directory
 */
void cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: cd <directory>\n");
        return;
    }
    
    vfs_node_t* target;
    
    /* Handle special cases */
    if (string_compare(argv[1], "/") == 0) {
        /* Go to root */
        target = vfs_get_root();
    } else if (string_compare(argv[1], ".") == 0) {
        /* Stay in current directory */
        return;
    } else if (string_compare(argv[1], "..") == 0) {
        /* Go to parent directory */
        vfs_node_t* current = kernel_get_current_directory();
        if (current && current->parent) {
            target = current->parent;
        } else {
            target = vfs_get_root();
        }
    } else if (argv[1][0] == '/') {
        /* Absolute path */
        target = vfs_resolve_path(argv[1]);
    } else {
        /* Relative path - build absolute path */
        char abs_path[VFS_MAX_PATH_LENGTH];
        build_absolute_path(argv[1], abs_path, VFS_MAX_PATH_LENGTH);
        target = vfs_resolve_path(abs_path);
    }
    
    if (!target) {
        console_write("cd: ");
        console_write(argv[1]);
        console_write(": No such file or directory\n");
        return;
    }
    
    if (target->type != NODE_DIRECTORY) {
        console_write("cd: ");
        console_write(argv[1]);
        console_write(": Not a directory\n");
        return;
    }
    
    kernel_set_current_directory(target);
}

/*
 * CAT command - Display file contents
 * Usage: cat [-n] [-h|--help] file [file...]
 */
void cmd_cat(int argc, char** argv) {
    /* Static buffer to avoid large stack allocation */
    static uint8_t buffer[VFS_MAX_FILE_SIZE];

    int flag_number = 0;
    int file_count = 0;

    /* First pass: check for help flag and count files */
    for (int i = 1; i < argc; i++) {
        if (string_compare(argv[i], "--help") == 0 || string_compare(argv[i], "-h") == 0) {
            console_write("Usage: cat [-n] [-h] file [file...]\n");
            console_write("  -n    number output lines\n");
            console_write("  -h    show this help message\n");
            return;
        } else if (string_compare(argv[i], "-n") == 0) {
            flag_number = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            console_write("cat: invalid option '");
            console_write(argv[i]);
            console_write("'\nTry 'cat -h' for more information.\n");
            return;
        } else {
            file_count++;
        }
    }

    if (file_count == 0) {
        console_write("Usage: cat [-n] file [file...]\n");
        return;
    }

    /* Second pass: process each file */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;  /* skip flags */

        vfs_node_t* file;
        if (argv[i][0] == '/') {
            file = vfs_resolve_path(argv[i]);
        } else {
            char abs_path[VFS_MAX_PATH_LENGTH];
            build_absolute_path(argv[i], abs_path, VFS_MAX_PATH_LENGTH);
            file = vfs_resolve_path(abs_path);
        }

        if (!file) {
            console_write("cat: ");
            console_write(argv[i]);
            console_write(": No such file or directory\n");
            continue;
        }

        if (file->type != NODE_FILE) {
            console_write("cat: ");
            console_write(argv[i]);
            console_write(": Is a directory\n");
            continue;
        }

        ssize_t bytes_read = vfs_read(file, 0, file->length, buffer);
        if (bytes_read < 0) {
            console_write("cat: ");
            console_write(argv[i]);
            console_write(": Error reading file\n");
            continue;
        }

        if (flag_number) {
            /* Print with line numbers */
            uint32_t line_num = 1;
            int at_line_start = 1;
            for (ssize_t j = 0; j < bytes_read; j++) {
                if (at_line_start) {
                    print_number_padded(line_num, 6);
                    console_write("  ");
                    at_line_start = 0;
                }
                console_put_char((char)buffer[j]);
                if (buffer[j] == '\n') {
                    line_num++;
                    at_line_start = 1;
                }
            }
            /* Ensure last line (without trailing newline) still counted */
        } else {
            for (ssize_t j = 0; j < bytes_read; j++) {
                console_put_char((char)buffer[j]);
            }
        }
    }
}

/*
 * STAT command - Show file or directory metadata
 * Usage: stat [-h|--help] path
 */
void cmd_stat(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: stat [-h] path\n");
        return;
    }

    /* Check for help flag */
    for (int i = 1; i < argc; i++) {
        if (string_compare(argv[i], "--help") == 0 || string_compare(argv[i], "-h") == 0) {
            console_write("Usage: stat [-h] path\n");
            console_write("  Display metadata for a file or directory.\n");
            console_write("  Shows: name, type, size, and inode number.\n");
            console_write("  -h    show this help message\n");
            return;
        }
    }

    /* Find the last non-flag argument as path */
    const char* path_arg = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            path_arg = argv[i];
        }
    }

    if (!path_arg) {
        console_write("stat: missing operand\n");
        console_write("Try 'stat -h' for more information.\n");
        return;
    }

    vfs_node_t* node;
    if (path_arg[0] == '/') {
        node = vfs_resolve_path(path_arg);
    } else {
        char abs_path[VFS_MAX_PATH_LENGTH];
        build_absolute_path(path_arg, abs_path, VFS_MAX_PATH_LENGTH);
        node = vfs_resolve_path(abs_path);
    }

    if (!node) {
        console_write("stat: cannot stat '");
        console_write(path_arg);
        console_write("': No such file or directory\n");
        return;
    }

    console_write("  File: ");
    console_write(node->name);
    console_put_char('\n');

    console_write("  Type: ");
    if (node->type == NODE_DIRECTORY) {
        console_write("directory\n");
    } else {
        console_write("regular file\n");
    }

    /* Use print_number (uint32_t, no int cast) for safe display */
    console_write("  Size: ");
    print_number(node->length);
    console_write(" bytes\n");

    console_write(" Inode: ");
    print_number(node->inode);
    console_put_char('\n');
}

/*
 * Helper: split a path into its parent directory node and the final
 * component ("leaf"). For a bare name (no '/'), the parent is the current
 * working directory. Returns the parent directory node, or NULL if the
 * parent path does not resolve to a directory. On success, leaf_out holds
 * the final path component.
 */
static vfs_node_t* resolve_parent_and_leaf(const char* path, char* leaf_out, size_t leaf_size) {
    /* Find the last '/' to separate parent path from leaf name. */
    int last_slash = -1;
    int len = (int)string_length(path);
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') {
            last_slash = i;
        }
    }

    if (last_slash < 0) {
        /* No slash: leaf is the whole string, parent is current directory. */
        size_t i = 0;
        for (; path[i] != '\0' && i < leaf_size - 1; i++) {
            leaf_out[i] = path[i];
        }
        leaf_out[i] = '\0';
        return kernel_get_current_directory();
    }

    /* Copy the leaf (portion after the last slash). */
    {
        const char* leaf_src = path + last_slash + 1;
        size_t i = 0;
        for (; leaf_src[i] != '\0' && i < leaf_size - 1; i++) {
            leaf_out[i] = leaf_src[i];
        }
        leaf_out[i] = '\0';
    }

    /* Resolve the parent path. */
    vfs_node_t* parent;
    if (last_slash == 0) {
        /* Path like "/foo": parent is root. */
        parent = vfs_get_root();
    } else {
        char parent_path[VFS_MAX_PATH_LENGTH];
        int i = 0;
        for (; i < last_slash && i < (int)sizeof(parent_path) - 1; i++) {
            parent_path[i] = path[i];
        }
        parent_path[i] = '\0';

        if (parent_path[0] == '/') {
            parent = vfs_resolve_path(parent_path);
        } else {
            char abs_path[VFS_MAX_PATH_LENGTH];
            build_absolute_path(parent_path, abs_path, VFS_MAX_PATH_LENGTH);
            parent = vfs_resolve_path(abs_path);
        }
    }

    if (!parent || parent->type != NODE_DIRECTORY) {
        return NULL;
    }
    return parent;
}

/*
 * MEMINFO command - Report physical (PMM) and heap memory usage.
 * Usage: meminfo
 */
void cmd_meminfo(int argc, char** argv) {
    (void)argc;
    (void)argv;

    struct pmm_stats pmm;
    pmm_get_stats(&pmm);

    console_write("Physical memory (PMM, 4 KiB pages)\n");
    console_write("  total : ");
    print_number(pmm.total_pages);
    console_write(" pages (");
    print_number(pmm.total_memory_kb);
    console_write(" KiB)\n");
    console_write("  used  : ");
    print_number(pmm.used_pages);
    console_write(" pages (");
    print_number(pmm.used_memory_kb);
    console_write(" KiB)\n");
    console_write("  free  : ");
    print_number(pmm.free_pages);
    console_write(" pages (");
    print_number(pmm.free_memory_kb);
    console_write(" KiB)\n");

    size_t h_total = 0, h_used = 0, h_free = 0;
    uint32_t h_blocks = 0;
    heap_get_stats(&h_total, &h_used, &h_free, &h_blocks);

    console_write("Kernel heap (first-fit free list)\n");
    console_write("  total : ");
    print_number((uint32_t)h_total);
    console_write(" bytes\n");
    console_write("  used  : ");
    print_number((uint32_t)h_used);
    console_write(" bytes\n");
    console_write("  free  : ");
    print_number((uint32_t)h_free);
    console_write(" bytes across ");
    print_number(h_blocks);
    console_write(" block(s)\n");
}

/*
 * MKDIR command - Create a new directory.
 * Usage: mkdir <name>
 */
void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: mkdir <name>\n");
        return;
    }

    for (int a = 1; a < argc; a++) {
        char leaf[VFS_NAME_LENGTH];
        vfs_node_t* parent = resolve_parent_and_leaf(argv[a], leaf, sizeof(leaf));
        if (!parent) {
            console_write("mkdir: ");
            console_write(argv[a]);
            console_write(": No such file or directory\n");
            continue;
        }
        if (leaf[0] == '\0') {
            console_write("mkdir: invalid directory name\n");
            continue;
        }
        if (vfs_find_node(parent, leaf) != NULL) {
            console_write("mkdir: ");
            console_write(argv[a]);
            console_write(": File exists\n");
            continue;
        }

        vfs_node_t* dir = vfs_mkchild(parent, leaf, NODE_DIRECTORY);
        if (!dir) {
            console_write("mkdir: cannot create directory (out of space)\n");
        }
    }
}

/*
 * TOUCH command - Create an empty file if it does not already exist.
 * Usage: touch <name>
 */
void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: touch <name>\n");
        return;
    }

    for (int a = 1; a < argc; a++) {
        char leaf[VFS_NAME_LENGTH];
        vfs_node_t* parent = resolve_parent_and_leaf(argv[a], leaf, sizeof(leaf));
        if (!parent) {
            console_write("touch: ");
            console_write(argv[a]);
            console_write(": No such file or directory\n");
            continue;
        }
        if (leaf[0] == '\0') {
            console_write("touch: invalid file name\n");
            continue;
        }
        /* Existing node: nothing to do (matches touch semantics loosely). */
        if (vfs_find_node(parent, leaf) != NULL) {
            continue;
        }

        vfs_node_t* file = vfs_mkchild(parent, leaf, NODE_FILE);
        if (!file) {
            console_write("touch: cannot create file (out of space)\n");
        }
    }
}

/*
 * WRITE command - Write text to a file, creating it if needed.
 * Usage: write <file> <text...>
 * The text is everything after the filename, space-joined, plus a newline.
 */
void cmd_write(int argc, char** argv) {
    if (argc < 3) {
        console_write("Usage: write <file> <text...>\n");
        return;
    }

    char leaf[VFS_NAME_LENGTH];
    vfs_node_t* parent = resolve_parent_and_leaf(argv[1], leaf, sizeof(leaf));
    if (!parent) {
        console_write("write: ");
        console_write(argv[1]);
        console_write(": No such file or directory\n");
        return;
    }
    if (leaf[0] == '\0') {
        console_write("write: invalid file name\n");
        return;
    }

    vfs_node_t* file = vfs_find_node(parent, leaf);
    if (file && file->type != NODE_FILE) {
        console_write("write: ");
        console_write(argv[1]);
        console_write(": Is a directory\n");
        return;
    }
    if (!file) {
        file = vfs_mkchild(parent, leaf, NODE_FILE);
        if (!file) {
            console_write("write: cannot create file\n");
            return;
        }
    }

    /* Assemble the payload from argv[2..] into a static buffer. */
    static uint8_t buffer[VFS_MAX_FILE_SIZE];
    uint32_t pos = 0;
    for (int i = 2; i < argc && pos < VFS_MAX_FILE_SIZE; i++) {
        if (i > 2 && pos < VFS_MAX_FILE_SIZE) {
            buffer[pos++] = ' ';
        }
        const char* w = argv[i];
        for (uint32_t j = 0; w[j] != '\0' && pos < VFS_MAX_FILE_SIZE; j++) {
            buffer[pos++] = (uint8_t)w[j];
        }
    }
    if (pos < VFS_MAX_FILE_SIZE) {
        buffer[pos++] = '\n';
    }

    ssize_t written = vfs_write(file, 0, pos, buffer);
    if (written < 0) {
        console_write("write: error writing file\n");
        return;
    }
    /* Truncate the logical length to what we just wrote. */
    file->length = (uint32_t)written;
}

/*
 * RM command - Remove a file or (empty or non-empty) directory entry.
 * Usage: rm <name>
 */
void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: rm <name>\n");
        return;
    }

    for (int a = 1; a < argc; a++) {
        char leaf[VFS_NAME_LENGTH];
        vfs_node_t* parent = resolve_parent_and_leaf(argv[a], leaf, sizeof(leaf));
        if (!parent) {
            console_write("rm: ");
            console_write(argv[a]);
            console_write(": No such file or directory\n");
            continue;
        }
        if (leaf[0] == '\0' || string_compare(leaf, ".") == 0 ||
            string_compare(leaf, "..") == 0) {
            console_write("rm: refusing to remove '");
            console_write(argv[a]);
            console_write("'\n");
            continue;
        }
        if (vfs_find_node(parent, leaf) == NULL) {
            console_write("rm: ");
            console_write(argv[a]);
            console_write(": No such file or directory\n");
            continue;
        }
        if (vfs_unlink(parent, leaf) != 0) {
            console_write("rm: ");
            console_write(argv[a]);
            console_write(": failed to remove\n");
        }
    }
}

/*
 * Reboot command - Reboot the system
 */
void cmd_reboot(int argc, char** argv) {
    (void)argc;  /* Unused parameter */
    (void)argv;  /* Unused parameter */
    
    console_write("Rebooting system...\n");
    
    /*
     * Give time for message to display.
     * NOTE: Busy-wait loop timing is CPU-speed dependent.
     * On typical systems this provides ~100-500ms delay.
     */
    for (volatile int i = 0; i < 10000000; i++) {
        /* Busy wait */
    }
    
    /* Method 1: Use keyboard controller to reset */
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE);
    
    /* Method 2: Triple fault (fallback) */
    /* Create an invalid IDT descriptor */
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) invalid_idt = {0, 0};
    
    __asm__ __volatile__("cli");  /* Disable interrupts */
    __asm__ __volatile__("lidt %0" : : "m"(invalid_idt));  /* Load invalid IDT */
    __asm__ __volatile__("int $0x03");  /* Trigger interrupt */
    
    /* If we get here, the reboot failed */
    console_write("Reboot failed!\n");
}

/*
 * Test IPC command - Test pipes and message queues
 */
void cmd_test_ipc(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    console_write("\n=== Testing IPC Mechanisms ===\n");
    
    /* Test pipe */
    console_write("\n1. Testing Pipe:\n");
    pipe_t* pipe = pipe_create(1, 2);
    if (pipe) {
        console_write("   - Pipe created successfully\n");
        
        const char* test_data = "Hello from pipe!";
        int written = pipe_write(pipe, test_data, strlen(test_data) + 1);
        console_write("   - Wrote ");
        char buf[16];
        itoa(written, buf, 10);
        console_write(buf);
        console_write(" bytes to pipe\n");
        
        char read_buf[64];
        int read = pipe_read(pipe, read_buf, sizeof(read_buf));
        console_write("   - Read ");
        itoa(read, buf, 10);
        console_write(buf);
        console_write(" bytes from pipe: ");
        console_write(read_buf);
        console_write("\n");
        
        pipe_close(pipe);
        console_write("   - Pipe closed\n");
    } else {
        console_write("   - Failed to create pipe\n");
    }
    
    /* Test message queue */
    console_write("\n2. Testing Message Queue:\n");
    msg_queue_t* queue = msgqueue_create(1);
    if (queue) {
        console_write("   - Message queue created successfully\n");
        
        const char* msg_data = "Test message";
        if (msgqueue_send(queue, 1, 100, msg_data, strlen(msg_data) + 1) == 0) {
            console_write("   - Sent message to queue\n");
            
            message_t msg;
            if (msgqueue_receive(queue, &msg) == 0) {
                console_write("   - Received message: ");
                console_write((const char*)msg.data);
                console_write("\n");
            }
        }
        
        msgqueue_close(queue);
        console_write("   - Message queue closed\n");
    } else {
        console_write("   - Failed to create message queue\n");
    }
    
    console_write("\nIPC test complete!\n\n");
}

/*
 * Test SMP command - Test multi-core support
 */
void cmd_test_smp(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    console_write("\n=== Testing Multi-core SMP Support ===\n\n");
    
    uint32_t cpu_count = smp_get_cpu_count();
    console_write("Detected CPU cores: ");
    char buf[16];
    itoa(cpu_count, buf, 10);
    console_write(buf);
    console_write("\n");
    
    uint32_t current_cpu = smp_get_current_cpu();
    console_write("Current CPU ID: ");
    itoa(current_cpu, buf, 10);
    console_write(buf);
    console_write("\n");
    
    console_write("\nCPU Information:\n");
    for (uint32_t i = 0; i < cpu_count; i++) {
        cpu_info_t* info = smp_get_cpu_info(i);
        if (info) {
            console_write("  CPU ");
            itoa(i, buf, 10);
            console_write(buf);
            console_write(": ");
            
            switch (info->state) {
                case CPU_STATE_ONLINE:
                    console_write("ONLINE");
                    break;
                case CPU_STATE_OFFLINE:
                    console_write("OFFLINE");
                    break;
                case CPU_STATE_HALTED:
                    console_write("HALTED");
                    break;
            }
            console_write("\n");
        }
    }
    
    console_write("\nSMP test complete!\n\n");
}

/*
 * Test GUI command - Test windowing system
 */
void cmd_test_gui(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    console_write("\n=== Testing GUI/Windowing System ===\n\n");
    
    console_write("Creating test window...\n");
    window_t* window = gui_create_window(100, 100, 400, 300, "Test Window");
    if (window) {
        console_write("Window created with ID: ");
        char buf[16];
        itoa(window->id, buf, 10);
        console_write(buf);
        console_write("\n");
        
        console_write("Showing window...\n");
        gui_show_window(window);
        
        console_write("Rendering window...\n");
        gui_render_window(window);

        console_write("Flipping to the real display (if a framebuffer is active)...\n");
        gui_flip_to_screen();

        console_write("Hiding window...\n");
        gui_hide_window(window);
        
        console_write("Destroying window...\n");
        gui_destroy_window(window);
        
        console_write("Window operations complete!\n");
    } else {
        console_write("Failed to create window\n");
    }
    
    console_write("\nGUI test complete!\n\n");
}

/*
 * Parse a dotted-decimal IPv4 address string ("a.b.c.d") into an
 * ip_addr_t. Returns 0 on success, -1 on any malformed input.
 */
static int parse_ipv4(const char* s, ip_addr_t* out) {
    int part = 0;
    int val = 0;
    int have_digit = 0;

    for (const char* p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            have_digit = 1;
            if (val > 255) return -1;
        } else if (*p == '.' || *p == '\0') {
            if (!have_digit || part >= 4) return -1;
            out->addr[part++] = (uint8_t)val;
            val = 0;
            have_digit = 0;
            if (*p == '\0') break;
        } else {
            return -1;
        }
    }

    return (part == 4) ? 0 : -1;
}

/*
 * PING command - send a real ICMP echo request over the NIC and wait
 * for the reply (kernel/network.c net_ping).
 * Usage: ping <ipv4-address>
 */
void cmd_ping(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: ping <ipv4-address>\n");
        console_write("  e.g. ping 10.0.2.2   (QEMU user-mode networking's default gateway)\n");
        return;
    }

    ip_addr_t target;
    if (parse_ipv4(argv[1], &target) != 0) {
        console_write("ping: invalid IPv4 address: ");
        console_write(argv[1]);
        console_write("\n");
        return;
    }

    net_device_t* dev = net_get_device();
    if (!dev || !dev->is_up) {
        console_write("ping: network is down (no RTL8139 NIC found at boot)\n");
        return;
    }

    console_write("PING ");
    console_write(argv[1]);
    console_write(": ");

    uint32_t rtt_ms = 0;
    if (net_ping(&target, &rtt_ms) == 0) {
        console_write("reply in ");
        char buf[12];
        itoa((int)rtt_ms, buf, 10);
        console_write(buf);
        console_write(" ms\n");
    } else {
        console_write("timeout (no reply within 1s)\n");
    }
}

/*
 * Parse an unsigned decimal integer. Returns 0 on success (and fills
 * *out), -1 on any non-digit character or an empty string.
 */
static int parse_uint_arg(const char* s, uint32_t* out) {
    if (!s || !*s) return -1;
    uint32_t v = 0;
    for (const char* p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    *out = v;
    return 0;
}

/*
 * TCPCONNECT command - demonstrate the minimal client TCP state machine
 * (kernel/network.c): a real 3-way handshake, optional data send, a brief
 * poll for a reply, then a FIN close.
 * Usage: tcpconnect <ipv4-address> <port> [message]
 */
void cmd_tcpconnect(int argc, char** argv) {
    if (argc < 3) {
        console_write("Usage: tcpconnect <ipv4-address> <port> [message]\n");
        console_write("  e.g. tcpconnect 10.0.2.2 7    (QEMU SLIRP has no echo service by default)\n");
        return;
    }

    ip_addr_t target;
    if (parse_ipv4(argv[1], &target) != 0) {
        console_write("tcpconnect: invalid IPv4 address: ");
        console_write(argv[1]);
        console_write("\n");
        return;
    }

    uint32_t port;
    if (parse_uint_arg(argv[2], &port) != 0 || port == 0 || port > 65535) {
        console_write("tcpconnect: invalid port: ");
        console_write(argv[2]);
        console_write("\n");
        return;
    }

    net_device_t* dev = net_get_device();
    if (!dev || !dev->is_up) {
        console_write("tcpconnect: network is down (no RTL8139 NIC found at boot)\n");
        return;
    }

    socket_t* sock = net_socket_create(PROTO_TCP);
    if (!sock) {
        console_write("tcpconnect: could not create socket\n");
        return;
    }

    console_write("Connecting to ");
    console_write(argv[1]);
    console_write(":");
    console_write(argv[2]);
    console_write("...\n");

    if (net_socket_connect(sock, &target, (uint16_t)port) != 0) {
        console_write("tcpconnect: connection failed (no SYN-ACK within 1s, or RST received)\n");
        net_socket_close(sock);
        return;
    }
    console_write("Connected.\n");

    if (argc >= 4) {
        int sent = net_socket_send(sock, argv[3], string_length(argv[3]));
        if (sent < 0) {
            console_write("tcpconnect: send failed\n");
        } else {
            console_write("Sent ");
            char buf[12];
            itoa(sent, buf, 10);
            console_write(buf);
            console_write(" byte(s)\n");
        }

        /* Brief best-effort poll for a reply - this stack has no blocking
         * recv(), so poll net_socket_recv()'s non-blocking "0 = nothing
         * queued yet" return for up to ~1s. */
        char reply[SOCK_RX_MAX_LEN + 1];
        int got = 0;
        uint64_t deadline = timer_get_ticks() + 100;
        while (timer_get_ticks() < deadline) {
            got = net_socket_recv(sock, reply, sizeof(reply) - 1);
            if (got > 0) break;
            __asm__ __volatile__("pause");
        }
        if (got > 0) {
            reply[got] = '\0';
            console_write("Reply (");
            char buf[12];
            itoa(got, buf, 10);
            console_write(buf);
            console_write(" byte(s)): ");
            console_write(reply);
            console_write("\n");
        } else {
            console_write("No reply within 1s.\n");
        }
    }

    net_socket_close(sock);
    console_write("Closed.\n");
}

/*
 * Test networking command - Test TCP/IP stack
 */
void cmd_test_net(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    console_write("\n=== Testing Networking Stack ===\n\n");
    
    net_device_t* dev = net_get_device();
    if (dev) {
        console_write("Network device: ");
        console_write(dev->name);
        console_write("\n");
        
        console_write("IP address: ");
        char buf[16];
        for (int i = 0; i < 4; i++) {
            itoa(dev->ip.addr[i], buf, 10);
            console_write(buf);
            if (i < 3) console_write(".");
        }
        console_write("\n");
        
        console_write("MAC address: ");
        for (int i = 0; i < 6; i++) {
            /* Format as two-digit hex with leading zero */
            unsigned int byte = dev->mac.addr[i];
            char hex_buf[3];
            hex_buf[0] = "0123456789abcdef"[byte >> 4];
            hex_buf[1] = "0123456789abcdef"[byte & 0xF];
            hex_buf[2] = '\0';
            console_write(hex_buf);
            if (i < 5) console_write(":");
        }
        console_write("\n");
        
        console_write("Status: ");
        console_write(dev->is_up ? "UP" : "DOWN");
        console_write("\n");
    }
    
    console_write("\nCreating test socket...\n");
    socket_t* sock = net_socket_create(PROTO_TCP);
    if (sock) {
        console_write("Socket created successfully\n");
        
        console_write("Binding to port 8080...\n");
        if (net_socket_bind(sock, 8080) == 0) {
            console_write("Socket bound successfully\n");
        }
        
        console_write("Closing socket...\n");
        net_socket_close(sock);
    } else {
        console_write("Failed to create socket\n");
    }
    
    console_write("\nNetworking test complete!\n\n");
}

/*
 * Test scripting command - Test shell scripting
 */
void cmd_test_script(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    console_write("\n=== Testing Shell Scripting ===\n\n");
    
    console_write("Setting test variables...\n");
    script_set_var("TEST_VAR", "Hello World");
    script_set_var("VERSION", "1.0");
    
    console_write("Reading variables:\n");
    const char* val = script_get_var("TEST_VAR");
    if (val) {
        console_write("  TEST_VAR = ");
        console_write(val);
        console_write("\n");
    }
    
    val = script_get_var("VERSION");
    if (val) {
        console_write("  VERSION = ");
        console_write(val);
        console_write("\n");
    }
    
    console_write("\nExecuting test script:\n");
    const char* test_script = 
        "NAME=OrgitoOS\n"
        "echo Starting script\n"
        "if true\n"
        "echo Condition is true\n";
    
    script_execute(test_script);

    console_write("\nScripting test complete!\n\n");
}

/*
 * GREP command - print lines containing a pattern.
 * Usage: grep <pattern> [file]
 *
 * Pipe-aware: when run as a non-first stage of a `cmd1 | grep pattern`
 * pipeline, reads the previous stage's captured output via
 * shell_stdin_read_line() instead of requiring a file argument.
 */
void cmd_grep(int argc, char** argv) {
    if (argc < 2) {
        console_write("Usage: grep <pattern> [file]\n");
        console_write("  Reads stdin when used in a pipeline: cmd | grep <pattern>\n");
        return;
    }

    const char* pattern = argv[1];
    char line[256];

    if (shell_stdin_available()) {
        while (shell_stdin_read_line(line, sizeof(line))) {
            if (strstr(line, pattern) != 0) {
                console_write(line);
                console_write("\n");
            }
        }
        return;
    }

    if (argc < 3) {
        console_write("grep: no input - provide a file or use a pipeline\n");
        return;
    }

    vfs_node_t* file;
    if (argv[2][0] == '/') {
        file = vfs_resolve_path(argv[2]);
    } else {
        char abs_path[VFS_MAX_PATH_LENGTH];
        build_absolute_path(argv[2], abs_path, VFS_MAX_PATH_LENGTH);
        file = vfs_resolve_path(abs_path);
    }

    if (!file || file->type != NODE_FILE) {
        console_write("grep: ");
        console_write(argv[2]);
        console_write(": No such file or directory\n");
        return;
    }

    static uint8_t grep_buffer[VFS_MAX_FILE_SIZE];
    ssize_t bytes_read = vfs_read(file, 0, file->length, grep_buffer);
    if (bytes_read < 0) {
        console_write("grep: error reading file\n");
        return;
    }

    size_t pos = 0;
    while ((ssize_t)pos < bytes_read) {
        size_t start = pos;
        while ((ssize_t)pos < bytes_read && grep_buffer[pos] != '\n') {
            pos++;
        }
        size_t len = pos - start;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, &grep_buffer[start], len);
        line[len] = '\0';

        if (strstr(line, pattern) != 0) {
            console_write(line);
            console_write("\n");
        }

        if ((ssize_t)pos < bytes_read) {
            pos++; /* skip the '\n' */
        }
    }
}

/*
 * WC command - count lines, words, and bytes.
 * Usage: wc [file]
 *
 * Pipe-aware: reads the previous stage's output in a `cmd | wc` pipeline;
 * otherwise reads a file argument.
 */
void cmd_wc(int argc, char** argv) {
    uint32_t lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    if (shell_stdin_available()) {
        char line[256];
        while (shell_stdin_read_line(line, sizeof(line))) {
            lines++;
            bytes += (uint32_t)string_length(line) + 1; /* +1 for the '\n' delimiter */
            in_word = 0;
            for (size_t i = 0; line[i] != '\0'; i++) {
                if (is_whitespace(line[i])) {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    words++;
                }
            }
        }
    } else {
        if (argc < 2) {
            console_write("Usage: wc [file]\n");
            console_write("  Reads stdin when used in a pipeline: cmd | wc\n");
            return;
        }

        vfs_node_t* file;
        if (argv[1][0] == '/') {
            file = vfs_resolve_path(argv[1]);
        } else {
            char abs_path[VFS_MAX_PATH_LENGTH];
            build_absolute_path(argv[1], abs_path, VFS_MAX_PATH_LENGTH);
            file = vfs_resolve_path(abs_path);
        }

        if (!file || file->type != NODE_FILE) {
            console_write("wc: ");
            console_write(argv[1]);
            console_write(": No such file or directory\n");
            return;
        }

        static uint8_t wc_buffer[VFS_MAX_FILE_SIZE];
        ssize_t bytes_read = vfs_read(file, 0, file->length, wc_buffer);
        if (bytes_read < 0) {
            console_write("wc: error reading file\n");
            return;
        }

        bytes = (uint32_t)bytes_read;
        for (ssize_t i = 0; i < bytes_read; i++) {
            if (wc_buffer[i] == '\n') {
                lines++;
            }
            if (is_whitespace((char)wc_buffer[i])) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    print_number(lines);
    console_write(" ");
    print_number(words);
    console_write(" ");
    print_number(bytes);
    console_write("\n");
}
