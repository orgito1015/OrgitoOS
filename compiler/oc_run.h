#ifndef ORGITOOS_COMPILER_OC_RUN_H
#define ORGITOOS_COMPILER_OC_RUN_H

#include <stddef.h>

/*
 * Compile and immediately run OrgitoCompiler (.oc) source already read into
 * memory (the `occ` shell command, kernel/commands.c, reads the file from
 * the VFS and passes the buffer here - this replaces OrionCC's original
 * fopen/fread-based main.c). `source` need not be NUL-terminated by the
 * caller; a NUL-terminated working copy is made internally. Mirrors the
 * original CLI's mode detection and output exactly: expression mode
 * (no ';' or '{' in the source) prints "Result: <int>"; program mode runs
 * print(...) statements and int main(). All diagnostics go through
 * console_write() (see oc_platform.h). Never fails destructively - lexer/
 * parser/codegen/VM errors are caught and reported, and this function
 * always returns normally.
 */
void oc_compile_and_run(const char *source, size_t len);

#endif /* ORGITOOS_COMPILER_OC_RUN_H */
