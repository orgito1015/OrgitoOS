/*
 * OrgitoCompiler - kernel entry point (replaces OrionCC's original,
 * fopen/fread-based main.c). See oc_run.h.
 */
#include "oc_platform.h"
#include "oc_run.h"
#include "lexer.h"
#include "parser.h"
#include "codegen.h"
#include "vm.h"
#include "ast.h"

/* Decide whether the source uses statement syntax. Every statement form
 * requires a ';' and every program-mode source has a '{' (function bodies),
 * so the presence of either means program mode; their absence means the
 * legacy single-expression mode (which prints `Result: <int>`). */
static int is_program_source(const char *src) {
    return strchr(src, ';') != NULL || strchr(src, '{') != NULL;
}

void oc_compile_and_run(const char *source, size_t len) {
    char *src = (char *)oc_malloc(len + 1);
    if (!src) {
        oc_eprintf("occ: out of memory reading source\n");
        return;
    }
    memcpy(src, source, len);
    src[len] = '\0';

    /*
     * Every lexer/parser/codegen/VM error in the ported pipeline goes
     * through OC_FAIL (oc_platform.h), which prints the same diagnostic
     * OrionCC always did and then __builtin_longjmp's back to here instead
     * of calling exit() - this is the one place that catches it. On the
     * error path, ast/bytecode nodes allocated before the failure are
     * intentionally not walked and freed here (there are ~150 OC_FAIL call
     * sites scattered across partially-built trees; doing this precisely
     * everywhere would be a much larger undertaking than this port
     * warrants) - only vm_execute()'s two large buffers get explicit
     * cleanup (oc_vm_cleanup_after_error(), the dominant leak source,
     * ~268 KiB per run vs. a few KiB at most for a failed parse/codegen).
     */
    if (__builtin_setjmp(g_oc_jmp_buf)) {
        oc_vm_cleanup_after_error();
        oc_free(src);
        return;
    }

    Lexer lex;
    lexer_init(&lex, src);

    Parser parser;
    parser_init(&parser, &lex);

    Bytecode bc;
    bc_init(&bc);

    if (is_program_source(src)) {
        /* Program mode: one or more function declarations; execution starts
         * at `int main()`. parse_program consumes tokens through EOF, so no
         * separate trailing-token check is needed here. */
        FuncList program = parse_program(&parser);

        codegen_program(&bc, &program);

        /* Program mode produces output via print() statements. The VM's
         * return value is main's return value, unused here (matching the
         * original CLI, which also discards it - the `Result: <int>` line
         * belongs to expression mode only). */
        (void)vm_execute(&bc);

        funclist_free(&program);
    } else {
        Expr *expr = parse_expression(&parser);
        if (lex.current_token.kind != TOK_EOF) {
            oc_eprintf("Error: extra tokens after valid expression\n");
            ast_free(expr);
            bc_free(&bc);
            oc_free(src);
            return;
        }
        codegen_expr(&bc, expr);
        codegen_finish(&bc);

        int result = vm_execute(&bc);
        oc_eprintf("Result: %d\n", result);

        ast_free(expr);
    }

    bc_free(&bc);
    oc_free(src);
}
