/*
 * OrgitoCompiler - freestanding-kernel platform shim
 *
 * OrgitoCompiler (ported from the standalone "OrionCC" project) was
 * originally a hosted CLI tool using malloc/free/realloc, fprintf/printf,
 * exit(), <limits.h>, <ctype.h>, and file I/O. This header collects every
 * substitution needed to run it inside OrgitoOS2's freestanding kernel
 * (-ffreestanding -nostdlib), in one place, so lexer.c/parser.c/ast.c/
 * codegen.c/vm.c stay otherwise close to the original.
 *
 *   - malloc/free  -> kmalloc/kfree (memory/heap.h), already matching
 *                     signatures.
 *   - realloc      -> oc_realloc() below (kmalloc + memcpy + kfree; no
 *                     kernel equivalent exists).
 *   - fprintf(stderr, ...); exit(EXIT_FAILURE); -> OC_FAIL(...): prints via
 *     oc_eprintf() (console_write-backed) then unwinds with
 *     __builtin_longjmp() instead of actually terminating a process. Every
 *     call site that jumps through OC_FAIL is inside oc_compile_and_run()'s
 *     __builtin_setjmp() catch point (oc_run.c), so control returns cleanly
 *     to the `occ` shell command - never a real exit().
 *   - <setjmp.h>   -> confirmed this fails to compile under this toolchain
 *     (missing 32-bit glibc headers on this box) even though the freestanding
 *     build otherwise works; GCC's __builtin_setjmp/__builtin_longjmp are
 *     pure compiler builtins with no header/library dependency at all, and
 *     were empirically verified to compile and produce sane code under the
 *     exact -ffreestanding -m32 -nostdlib flags this kernel uses.
 *   - <limits.h>   -> also confirmed to fail here (same missing headers);
 *     OC_INT_MIN/OC_INT_MAX below are the two constants actually used.
 *   - <ctype.h>    -> oc_isdigit/oc_isalpha/oc_isalnum/oc_isspace below.
 *   - strlen/strcmp/strncpy/memcpy/memset -> already provided with matching
 *     signatures by kernel/string.h; just #include it, no shim needed.
 *   - fopen/fread/FILE (host file I/O, only ever used in the original
 *     main.c) -> not shimmed here at all; oc_run.c reads source from the
 *     VFS instead (see kernel/commands.c's cmd_occ, which reads the file
 *     and passes the buffer straight to oc_compile_and_run()).
 */

#ifndef ORGITOOS_COMPILER_OC_PLATFORM_H
#define ORGITOOS_COMPILER_OC_PLATFORM_H

#include <stddef.h>
#include <stdarg.h>
#include "../memory/heap.h"
#include "../drivers/console.h"
#include "../kernel/string.h"

/* ---- allocation ---- */
#define oc_malloc  kmalloc
#define oc_free    kfree

void *oc_realloc(void *ptr, size_t old_size, size_t new_size);

/* ---- ctype subset actually used by the lexer ---- */
static inline int oc_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int oc_isalpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline int oc_isalnum(int c) { return oc_isalpha(c) || oc_isdigit(c); }
static inline int oc_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* ---- <limits.h> substitutes (the only two constants used) ---- */
#define OC_INT_MIN (-2147483647 - 1)
#define OC_INT_MAX  2147483647

/*
 * ---- mini formatted output ----
 *
 * Supports exactly the specifiers OrgitoCompiler's diagnostics and print()
 * runtime actually use (confirmed by grep over the original source):
 * %d (int), %lld (long long), %zu (size_t), %s (const char*), %c (char),
 * %%. Output goes through console_write(), so it lands in whichever
 * split-screen pane the calling shell instance owns.
 */
void oc_eprintf(const char *fmt, ...);

/*
 * ---- error unwinding ----
 *
 * g_oc_error_jmp is set up once per oc_compile_and_run() call (oc_run.c).
 * OC_FAIL(...) is the direct replacement for every
 * `fprintf(stderr, ...); exit(EXIT_FAILURE);` pair in the ported source:
 * print the same message, then longjmp back to the catch point instead of
 * terminating. __builtin_longjmp requires the target __builtin_setjmp's
 * enclosing function to still be on the stack, which is always true here -
 * oc_compile_and_run() never returns before the pipeline finishes.
 */
extern void *g_oc_jmp_buf[5];

#define OC_FAIL(...) \
    do { oc_eprintf(__VA_ARGS__); __builtin_longjmp(g_oc_jmp_buf, 1); } while (0)

#endif /* ORGITOOS_COMPILER_OC_PLATFORM_H */
