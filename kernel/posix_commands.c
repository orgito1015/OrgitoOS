/*
 * OrgitoOS - POSIX file-I/O shell commands
 *
 *   exec <program>           run an ELF program and show where it was loaded
 *   strace [-o f] [-e trace=open,read,write,close] [-V] <cmd> [args]
 *                            run <cmd> (usually a program such as FileCopy)
 *                            and record every file syscall it makes
 *                            through int 0x80, in strace's format
 *   chmod <mode> <file>      set permission bits (e.g. 000, 444, 644)
 *   cmp <file1> <file2>      compare two files byte by byte
 *   whoami                   print the (single) user name
 *   layout full|split        80-column console, or the default split panes
 *
 * Programs such as FileCopy are separate ELF files in /bin (built from
 * userland/, loaded by kernel/exec.c) that run as their own ring 3
 * processes; the shell starts them for any name that is not a built-in.
 */

#include "commands.h"
#include "shell.h"
#include "string.h"
#include "kernel.h"
#include "fileio.h"
#include "../fs/vfs.h"
#include "../process/process.h"
#include "../drivers/console.h"
#include "exec.h"
#include "../fs/diskfs.h"

static void print_uint(uint32_t v) {
    char buf[12];
    itoa((int)v, buf, 10);
    console_write(buf);
}

/* ------------------------------------------------------------------ */

static void cmd_exec(int argc, char **argv) {
    if (argc != 2) {
        console_write("usage: exec <program>   (e.g. exec /bin/hello)\n");
        shell_set_status(1);
        return;
    }
    int err = 0;
    process_t *p = process_exec(argv[1], PRIORITY_NORMAL, 1, &err);
    if (!p) {
        console_write("exec: ");
        console_write(argv[1]);
        console_write(": ");
        console_write(exec_strerror(err));
        console_write("\n");
        shell_set_status(err == EXEC_ENOENT ? 127 : 126);
        return;
    }
    int pid = (int)p->pid;
    p->keep_zombie = 1;
    process_start(p);
    int status = 0;
    if (process_waitpid(pid, &status) < 0) status = 1;
    shell_set_status(status);
}

/* ------------------------------------------------------------------ */

static uint32_t parse_trace_set(const char *list) {
    uint32_t mask = 0;
    char tmp[96];
    strncpy(tmp, list, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *p = tmp;
    while (*p) {
        char *start = p;
        while (*p && *p != ',') p++;
        if (*p == ',') *p++ = '\0';
        if (!string_compare(start, "open") || !string_compare(start, "openat"))
            mask |= FILEIO_TRACE_OPEN;
        else if (!string_compare(start, "read"))
            mask |= FILEIO_TRACE_READ;
        else if (!string_compare(start, "write"))
            mask |= FILEIO_TRACE_WRITE;
        else if (!string_compare(start, "close"))
            mask |= FILEIO_TRACE_CLOSE;
        else if (!string_compare(start, "all") || !string_compare(start, "%file"))
            mask |= FILEIO_TRACE_ALL;
    }
    return mask;
}

static void cmd_strace(int argc, char **argv) {
    const char *outfile = 0;
    uint32_t mask = FILEIO_TRACE_ALL;
    int i = 1;

    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!string_compare(argv[i], "-V")) {
            console_write("strace -- OrgitoOS syscall tracer, version 1.0\n");
            console_write("traces: openat read write close (int 0x80)\n");
            return;
        } else if (!string_compare(argv[i], "-o") && i + 1 < argc) {
            outfile = argv[++i];
        } else if (!string_compare(argv[i], "-e") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strncmp(e, "trace=", 6)) e += 6;
            mask = parse_trace_set(e);
        } else {
            console_write("strace: unknown option ");
            console_write(argv[i]);
            console_write("\n");
            shell_set_status(1);
            return;
        }
    }
    if (i >= argc) {
        console_write("usage: strace [-o file] [-e trace=open,read,write,close] [-V] cmd\n");
        shell_set_status(1);
        return;
    }

    vfs_node_t *out = 0;
    if (outfile) {
        /* Create/truncate the trace file (kernel-internal, not traced) */
        int fd = fileio_open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            console_write("strace: cannot open ");
            console_write(outfile);
            console_write(": ");
            console_write(fileio_strerror(-fd));
            console_write("\n");
            shell_set_status(1);
            return;
        }
        fileio_close(fd);
        out = fileio_lookup(outfile);
    }

    process_t *p = process_current();
    p->trace_out  = out;
    p->trace_mask = mask;

    /* Run the command; a program started by it inherits the trace
     * settings (process_create_user_suspended), like strace -f. */
    shell_run_argv(argc - i, argv + i);
    int status = shell_get_status();

    /* "+++ exited with N +++" closes the trace, as in strace */
    char line[40];
    char num[12];
    itoa(status, num, 10);
    string_copy(line, "+++ exited with ");
    string_concat(line, num);
    string_concat(line, " +++");
    fileio_trace_text(line);

    p->trace_mask = 0;
    p->trace_out  = 0;
    shell_set_status(status);
}

/* ------------------------------------------------------------------ */

static void cmd_chmod(int argc, char **argv) {
    if (argc != 3) {
        console_write("usage: chmod <mode> <file>   (e.g. chmod 000 secret.txt)\n");
        shell_set_status(1);
        return;
    }
    uint32_t mode = 0;
    for (const char *c = argv[1]; *c; c++) {
        if (*c < '0' || *c > '7') {
            console_write("chmod: invalid mode: ");
            console_write(argv[1]);
            console_write("\n");
            shell_set_status(1);
            return;
        }
        mode = (mode << 3) | (uint32_t)(*c - '0');
    }
    vfs_node_t *n = fileio_lookup(argv[2]);
    if (!n) {
        console_write("chmod: cannot access '");
        console_write(argv[2]);
        console_write("': No such file or directory\n");
        shell_set_status(1);
        return;
    }
    n->flags = VFS_FLAG_MODE_SET | (mode & VFS_MODE_MASK);
    diskfs_sync();   /* persist the new mode if the file is under /mnt */
}

/* ------------------------------------------------------------------ */

static void cmd_cmp(int argc, char **argv) {
    if (argc != 3) {
        console_write("usage: cmp <file1> <file2>\n");
        shell_set_status(2);
        return;
    }
    vfs_node_t *a = fileio_lookup(argv[1]);
    vfs_node_t *b = fileio_lookup(argv[2]);
    for (int k = 0; k < 2; k++) {
        vfs_node_t *n = k ? b : a;
        if (!n || n->type != NODE_FILE) {
            console_write("cmp: ");
            console_write(argv[1 + k]);
            console_write(n ? ": Is a directory\n" : ": No such file or directory\n");
            shell_set_status(2);
            return;
        }
    }

    /* Compare in chunks through vfs_read() (works for ramfs and disk). */
    static uint8_t ba[256], bb[256];
    uint32_t len = a->length < b->length ? a->length : b->length;
    uint32_t line = 1;
    for (uint32_t off = 0; off < len; off += sizeof(ba)) {
        uint32_t n = len - off < sizeof(ba) ? len - off : sizeof(ba);
        if (vfs_read(a, off, n, ba) != (ssize_t)n || vfs_read(b, off, n, bb) != (ssize_t)n) {
            console_write("cmp: read error\n");
            shell_set_status(2);
            return;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (ba[i] != bb[i]) {
                console_write(argv[1]);
                console_write(" ");
                console_write(argv[2]);
                console_write(" differ: byte ");
                print_uint(off + i + 1);
                console_write(", line ");
                print_uint(line);
                console_write("\n");
                shell_set_status(1);
                return;
            }
            if (ba[i] == '\n') line++;
        }
    }
    if (a->length != b->length) {
        console_write("cmp: EOF on ");
        console_write(a->length < b->length ? argv[1] : argv[2]);
        console_write("\n");
        shell_set_status(1);
        return;
    }
    shell_set_status(0);   /* identical: silent, like cmp */
}

/* ------------------------------------------------------------------ */

static void cmd_df(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (!diskfs_is_mounted()) {
        console_write("df: no disk mounted (start QEMU with disk.img)\n");
        shell_set_status(1);
        return;
    }
    uint32_t tb, fb, ti, fi;
    diskfs_stats(&tb, &fb, &ti, &fi);
    console_write("Filesystem  Size(KiB)  Used(KiB)  Free(KiB)  Inodes(free)  Mounted on\n");
    console_write("OrgitoFS    ");
    print_uint(tb / 2);
    console_write("      ");
    print_uint((tb - fb) / 2);
    console_write("        ");
    print_uint(fb / 2);
    console_write("      ");
    print_uint(ti);
    console_write("(");
    print_uint(fi);
    console_write(")       /mnt\n");
    console_write("Max file size on disk: ");
    print_uint(diskfs_max_file_size());
    console_write(" bytes\n");
}

static void cmd_sync(int argc, char **argv) {
    (void)argc;
    (void)argv;
    diskfs_sync();
}

static void cmd_whoami(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* OrgitoOS is single-user: everything runs as this account. */
    console_write("orgito\n");
}

static void cmd_layout(int argc, char **argv) {
    if (argc == 2 && !string_compare(argv[1], "full")) {
        console_set_fullscreen(1);
    } else if (argc == 2 && !string_compare(argv[1], "split")) {
        console_set_fullscreen(0);
    } else {
        console_write("usage: layout full|split\n");
        shell_set_status(1);
    }
}

/* ------------------------------------------------------------------ */

void posix_commands_register(void) {
    shell_register_command("exec", "Run an ELF program verbosely [exec /bin/hello]", cmd_exec);
    shell_register_command("strace", "Trace file syscalls [-o f] [-e trace=..] cmd", cmd_strace);
    shell_register_command("chmod", "Change file permissions [chmod 644 f]", cmd_chmod);
    shell_register_command("cmp", "Compare two files byte by byte", cmd_cmp);
    shell_register_command("whoami", "Print the current user name", cmd_whoami);
    shell_register_command("df", "Show disk (/mnt) usage", cmd_df);
    shell_register_command("sync", "Write disk metadata back to disk", cmd_sync);
    shell_register_command("layout", "Console layout [layout full|split]", cmd_layout);
}
