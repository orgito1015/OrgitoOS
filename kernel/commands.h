/*
 * OrgitoOS - Built-in Shell Commands
 * 
 * Declarations for all built-in shell commands.
 */

#ifndef ORGITOOS_KERNEL_COMMANDS_H
#define ORGITOOS_KERNEL_COMMANDS_H

/* Kernel release string reported by `uname -r` */
#define ORGITOOS_KERNEL_RELEASE "0.1.0-orgitoos-i386"

/* POSIX file-I/O commands: FileCopy, strace, chmod, cmp, whoami, layout
 * (kernel/posix_commands.c) */
void posix_commands_register(void);

/* Register all built-in commands */
void commands_register_all(void);

/* Individual command handlers */
void cmd_help(int argc, char** argv);
void cmd_clear(int argc, char** argv);
void cmd_echo(int argc, char** argv);
void cmd_uname(int argc, char** argv);
void cmd_uptime(int argc, char** argv);
void cmd_pwd(int argc, char** argv);
void cmd_ls(int argc, char** argv);
void cmd_cd(int argc, char** argv);
void cmd_cat(int argc, char** argv);
void cmd_stat(int argc, char** argv);
void cmd_mkdir(int argc, char** argv);
void cmd_touch(int argc, char** argv);
void cmd_write(int argc, char** argv);
void cmd_rm(int argc, char** argv);
void cmd_meminfo(int argc, char** argv);
void cmd_reboot(int argc, char** argv);
void cmd_color(int argc, char** argv);
void cmd_occ(int argc, char** argv);

/* New feature test commands */
void cmd_test_ipc(int argc, char** argv);
void cmd_test_smp(int argc, char** argv);
void cmd_test_gui(int argc, char** argv);
void cmd_test_net(int argc, char** argv);
void cmd_test_script(int argc, char** argv);

/* Process management commands (Phase 1, kernel/proc_commands.c) */
void cmd_ps(int argc, char** argv);
void cmd_kill(int argc, char** argv);
void cmd_spawn(int argc, char** argv);
void cmd_forktest(int argc, char** argv);
void cmd_user(int argc, char** argv);
void cmd_counters(int argc, char** argv);
void cmd_uecho(int argc, char** argv);
void cmd_psleep(int argc, char** argv);
void cmd_sched(int argc, char** argv);

#endif /* ORGITOOS_KERNEL_COMMANDS_H */
