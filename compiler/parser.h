#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

/*
 * Lowered from upstream OrionCC's 2000: that value was sized against a
 * hosted CLI's ~8 MiB thread stack. Ported into OrgitoOS2, this recursion
 * runs on a kernel thread's PROCESS_KSTACK_SIZE (16 KiB, process/process.h)
 * with no guard page - an overflow silently corrupts adjacent kernel
 * memory instead of faulting. 64 recursive parser frames comfortably fits
 * that budget while still allowing realistically nested expressions.
 */
#define PARSER_MAX_DEPTH 64

typedef struct {
    Lexer *lex;
    int    depth;      /* current recursion depth for expressions           */
    int    stmt_depth; /* current recursion depth for nested statements     */
    int    loop_depth; /* number of enclosing loops at the current position;
                        * used to reject break/continue outside any loop at
                        * parse time (see parser.c for why parse time)      */
} Parser;

void parser_init(Parser *p, Lexer *lex);

/* Legacy: parse a single expression (used by expression-only mode). */
Expr *parse_expression(Parser *p);

/* Parse a full program: one or more function declarations until EOF.
 * (Program mode requires an `int main()` entry point; enforced in codegen.) */
FuncList parse_program(Parser *p);

#endif /* PARSER_H */
