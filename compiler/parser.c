/* OrgitoCompiler parser - ported from OrionCC. See oc_platform.h for the
 * freestanding-kernel substitutions (exit->OC_FAIL, INT_MIN/MAX). Grammar
 * and parsing logic are otherwise unchanged from upstream; see parser.h
 * for the PARSER_MAX_DEPTH change (kernel-stack safety). */
#include "oc_platform.h"
#include "parser.h"

/* Expression grammar (lowest to highest precedence):
 *   expr       := logic_or
 *   logic_or   := logic_and ('||' logic_and)*
 *   logic_and  := equality  ('&&' equality)*
 *   equality   := relational (('==' | '!=') relational)*
 *   relational := additive (('<' | '>' | '<=' | '>=') additive)*
 *   additive   := term (('+' | '-') term)*
 *   term       := unary (('*' | '/' | '%') unary)*
 *   unary      := ('-' | '!') unary | primary
 *   primary    := NUMBER | 'true' | 'false'
 *              |  IDENT                      -- scalar variable
 *              |  IDENT '(' args? ')'        -- function call
 *              |  IDENT '[' expr ']'         -- array element (rvalue)
 *              |  '(' expr ')'
 *   args       := expr (',' expr)*
 *
 * Statement grammar:
 *   program    := funcdecl+                  -- program mode is functions only;
 *                                               an `int main()` must exist
 *   funcdecl   := 'int' IDENT '(' params? ')' '{' stmt* '}'
 *   params     := 'int' IDENT (',' 'int' IDENT)*
 *   stmt       := ('int' | 'bool') IDENT '=' expr ';'
 *              |  ('int' | 'bool') IDENT '[' expr ']' ';'   -- constant size
 *              |  'print' '(' expr ')' ';'
 *              |  'print' '(' STRING ')' ';'
 *              |  'if' '(' expr ')' stmt ('else' stmt)?
 *              |  'while' '(' expr ')' stmt
 *              |  'for' '(' forinit? ';' expr? ';' simple? ')' stmt
 *              |  'break' ';'
 *              |  'continue' ';'
 *              |  'return' expr? ';'
 *              |  '{' stmt* '}'
 *              |  simple ';'
 *   forinit    := ('int' | 'bool') IDENT '=' expr | simple
 *   simple     := IDENT '=' expr
 *              |  IDENT ('+='|'-='|'*='|'/='|'%=') expr   -- sugar: x = x op e
 *              |  IDENT ('++' | '--')                     -- sugar: x = x +/- 1
 *              |  ('++' | '--') IDENT                     -- sugar: x = x +/- 1
 *              |  IDENT '[' expr ']' '=' expr             -- array element store
 *              |  expr
 *
 * Notes on ++/--:
 *   They are statement-only sugar for `x = x + 1;` / `x = x - 1;` and do NOT
 *   produce a value, so they cannot appear inside expressions. One exception
 *   is kept for backwards compatibility: `--` applied to a *non-identifier*
 *   in an expression (e.g. `--5`) is read as two unary minuses, which is how
 *   it lexed before `--` became a single token. Applying `--` to a variable
 *   inside an expression is rejected, so it can never silently mean
 *   "negate twice" where a C programmer would expect a mutating decrement.
 */

static Expr *parse_logic_or(Parser *p);
static Expr *parse_logic_and(Parser *p);
static Expr *parse_equality(Parser *p);
static Expr *parse_relational(Parser *p);
static Expr *parse_additive(Parser *p);
static Expr *parse_term(Parser *p);
static Expr *parse_unary(Parser *p);
static Expr *parse_primary(Parser *p);
static Stmt *parse_stmt(Parser *p);

static void parser_error_at(const Token *tok, const char *msg) {
    OC_FAIL("Parse error at line %zu, column %zu: %s\n",
            tok->line, tok->col, msg);
}

static void parser_error(Parser *p, const char *msg) {
    parser_error_at(&p->lex->current_token, msg);
}

/* Allocation-failure handler: report and abort. Any partially-built AST is
 * freed by the caller via the `left`/`inner` bookkeeping below. */
static void check_alloc(Parser *p, void *ptr, Expr *a, Expr *b) {
    if (!ptr) {
        ast_free(a);
        ast_free(b);
        OC_FAIL("Parse error at line %zu, column %zu: out of memory\n",
                p->lex->current_token.line, p->lex->current_token.col);
    }
}

void parser_init(Parser *p, Lexer *lex) {
    p->lex = lex;
    p->depth = 0;
    p->stmt_depth = 0;
    p->loop_depth = 0;
}

static void depth_enter(Parser *p) {
    if (++p->depth > PARSER_MAX_DEPTH) {
        parser_error(p, "expression nesting too deep");
    }
}

static void depth_leave(Parser *p) {
    p->depth--;
}

/* ---------- expressions ---------- */

Expr *parse_expression(Parser *p) {
    return parse_logic_or(p);
}

static Expr *parse_logic_or(Parser *p) {
    Expr *left = parse_logic_and(p);
    while (p->lex->current_token.kind == TOK_OR) {
        lexer_next(p->lex);
        Expr *right = parse_logic_and(p);
        Expr *node = ast_binary(OP_OR, left, right);
        check_alloc(p, node, left, right);
        left = node;
    }
    return left;
}

static Expr *parse_logic_and(Parser *p) {
    Expr *left = parse_equality(p);
    while (p->lex->current_token.kind == TOK_AND) {
        lexer_next(p->lex);
        Expr *right = parse_equality(p);
        Expr *node = ast_binary(OP_AND, left, right);
        check_alloc(p, node, left, right);
        left = node;
    }
    return left;
}

static Expr *parse_equality(Parser *p) {
    Expr *left = parse_relational(p);
    while (p->lex->current_token.kind == TOK_EQ ||
           p->lex->current_token.kind == TOK_NE) {
        TokenKind op = p->lex->current_token.kind;
        lexer_next(p->lex);
        Expr *right = parse_relational(p);
        Expr *node = ast_binary(op == TOK_EQ ? OP_EQ : OP_NE, left, right);
        check_alloc(p, node, left, right);
        left = node;
    }
    return left;
}

static Expr *parse_relational(Parser *p) {
    Expr *left = parse_additive(p);
    while (p->lex->current_token.kind == TOK_LT ||
           p->lex->current_token.kind == TOK_GT ||
           p->lex->current_token.kind == TOK_LE ||
           p->lex->current_token.kind == TOK_GE) {
        TokenKind op = p->lex->current_token.kind;
        lexer_next(p->lex);
        Expr *right = parse_additive(p);
        BinOp bop = OP_LT;
        if (op == TOK_LT) bop = OP_LT;
        else if (op == TOK_GT) bop = OP_GT;
        else if (op == TOK_LE) bop = OP_LE;
        else bop = OP_GE;
        Expr *node = ast_binary(bop, left, right);
        check_alloc(p, node, left, right);
        left = node;
    }
    return left;
}

static Expr *parse_additive(Parser *p) {
    Expr *left = parse_term(p);
    while (p->lex->current_token.kind == TOK_PLUS ||
           p->lex->current_token.kind == TOK_MINUS) {
        TokenKind op = p->lex->current_token.kind;
        lexer_next(p->lex);
        Expr *right = parse_term(p);
        Expr *node = ast_binary(op == TOK_PLUS ? OP_PLUS : OP_MINUS, left, right);
        check_alloc(p, node, left, right);
        left = node;
    }
    return left;
}

static Expr *parse_term(Parser *p) {
    Expr *left = parse_unary(p);
    while (p->lex->current_token.kind == TOK_STAR ||
           p->lex->current_token.kind == TOK_SLASH ||
           p->lex->current_token.kind == TOK_PERCENT) {
        TokenKind op = p->lex->current_token.kind;
        lexer_next(p->lex);
        Expr *right = parse_unary(p);
        BinOp bop = (op == TOK_STAR) ? OP_MUL : (op == TOK_SLASH) ? OP_DIV : OP_MOD;
        Expr *node = ast_binary(bop, left, right);
        check_alloc(p, node, left, right);
        left = node;
    }
    return left;
}

static Expr *parse_unary(Parser *p) {
    /* Every expression-nesting level (a parenthesized subexpression, or one
     * link in a unary chain) passes through parse_unary exactly once, so a
     * single depth counter here bounds the true recursion depth and prevents
     * a stack overflow on pathological input (thousands of '(' or '-'). */
    depth_enter(p);

    if (p->lex->current_token.kind == TOK_MINUS) {
        lexer_next(p->lex);
        /* Special-case: -<NUMBER> where NUMBER is the INT_MIN magnitude
         * sentinel. The lexer stores INT_MAX+1 as INT_MIN sentinel in
         * int_value; a preceding unary minus makes it a valid INT_MIN. */
        if (p->lex->current_token.kind == TOK_NUMBER &&
            p->lex->current_token.int_value == OC_INT_MIN) {
            /* -(-2147483648 magnitude) == INT_MIN */
            lexer_next(p->lex);
            Expr *node = ast_number(OC_INT_MIN);
            check_alloc(p, node, NULL, NULL);
            depth_leave(p);
            return node;
        }
        Expr *inner = parse_unary(p);
        Expr *node = ast_unary(UOP_NEG, inner);
        check_alloc(p, node, inner, NULL);
        depth_leave(p);
        return node;
    }

    if (p->lex->current_token.kind == TOK_NOT) {
        lexer_next(p->lex);
        Expr *inner = parse_unary(p);
        Expr *node = ast_unary(UOP_NOT, inner);
        check_alloc(p, node, inner, NULL);
        depth_leave(p);
        return node;
    }

    /* '--' in an expression: legacy double-unary-minus reading for
     * non-identifier operands only (e.g. `--5` == 5). See the grammar
     * comment at the top of this file. */
    if (p->lex->current_token.kind == TOK_DECR) {
        lexer_next(p->lex);
        if (p->lex->current_token.kind == TOK_IDENT) {
            parser_error(p, "'--' on a variable is only supported as a statement (e.g. `x--;`)");
        }
        Expr *first;
        if (p->lex->current_token.kind == TOK_NUMBER &&
            p->lex->current_token.int_value == OC_INT_MIN) {
            /* '--2147483648' == -(-2147483648): the inner minus + sentinel
             * form INT_MIN, the outer minus is applied below. */
            lexer_next(p->lex);
            first = ast_number(OC_INT_MIN);
            check_alloc(p, first, NULL, NULL);
        } else {
            Expr *inner = parse_unary(p);
            first = ast_unary(UOP_NEG, inner);
            check_alloc(p, first, inner, NULL);
        }
        Expr *node = ast_unary(UOP_NEG, first);
        check_alloc(p, node, first, NULL);
        depth_leave(p);
        return node;
    }

    if (p->lex->current_token.kind == TOK_INCR) {
        parser_error(p, "'++' is only supported as a statement (e.g. `x++;`)");
    }

    Expr *e = parse_primary(p);
    depth_leave(p);
    return e;
}

static Expr *parse_primary(Parser *p) {
    Token tok = p->lex->current_token;

    if (tok.kind == TOK_NUMBER) {
        /* A bare INT_MAX+1 magnitude (sentinel INT_MIN) with no preceding
         * unary minus is out of range for a positive int. */
        if (tok.int_value == OC_INT_MIN) {
            parser_error_at(&tok, "integer literal out of range");
        }
        lexer_next(p->lex);
        Expr *node = ast_number(tok.int_value);
        check_alloc(p, node, NULL, NULL);
        return node;
    }

    if (tok.kind == TOK_KW_TRUE || tok.kind == TOK_KW_FALSE) {
        lexer_next(p->lex);
        Expr *node = ast_number(tok.kind == TOK_KW_TRUE ? 1 : 0);
        check_alloc(p, node, NULL, NULL);
        return node;
    }

    if (tok.kind == TOK_STRING) {
        parser_error_at(&tok, "string literals are only allowed as the sole argument of print(...)");
    }

    if (tok.kind == TOK_IDENT) {
        TokenKind pk = lexer_peek(p->lex)->kind;

        if (pk == TOK_LPAREN) {
            /* function call: IDENT '(' args? ')' */
            lexer_next(p->lex); /* consume IDENT */
            lexer_next(p->lex); /* consume '('   */
            ExprList args;
            exprlist_init(&args);
            if (p->lex->current_token.kind != TOK_RPAREN) {
                for (;;) {
                    Expr *a = parse_logic_or(p);
                    exprlist_push(&args, a);
                    if (p->lex->current_token.kind == TOK_COMMA) {
                        lexer_next(p->lex);
                        continue;
                    }
                    break;
                }
            }
            if (p->lex->current_token.kind != TOK_RPAREN) {
                exprlist_free(&args);
                parser_error(p, "expected ')' after function arguments");
            }
            lexer_next(p->lex);
            Expr *node = ast_call(tok.text, args);
            if (!node) {
                exprlist_free(&args);
                parser_error(p, "out of memory");
            }
            return node;
        }

        if (pk == TOK_LBRACKET) {
            /* array element: IDENT '[' expr ']' */
            lexer_next(p->lex); /* consume IDENT */
            lexer_next(p->lex); /* consume '['   */
            Expr *idx = parse_logic_or(p);
            if (p->lex->current_token.kind != TOK_RBRACKET) {
                ast_free(idx);
                parser_error(p, "expected ']' after array index");
            }
            lexer_next(p->lex);
            Expr *node = ast_index(tok.text, idx);
            check_alloc(p, node, idx, NULL);
            return node;
        }

        lexer_next(p->lex);
        Expr *node = ast_var(tok.text);
        check_alloc(p, node, NULL, NULL);
        return node;
    }

    if (tok.kind == TOK_LPAREN) {
        lexer_next(p->lex);
        Expr *e = parse_logic_or(p);
        if (p->lex->current_token.kind != TOK_RPAREN) {
            ast_free(e);
            parser_error(p, "expected ')'");
        }
        lexer_next(p->lex);
        return e;
    }

    parser_error(p, "unexpected token in expression");
    return NULL;
}

/* ---------- statements ---------- */

static void expect(Parser *p, TokenKind kind, const char *msg) {
    if (p->lex->current_token.kind != kind) {
        parser_error(p, msg);
    }
    lexer_next(p->lex);
}

static void copy_ident(char *dst, const Token *tok) {
    /* Token.text is a full LEX_MAX_IDENT buffer, always NUL-terminated by
     * the lexer, so a whole-buffer copy is exact (and avoids the
     * -Wstringop-truncation false positive strncpy would trigger). */
    memcpy(dst, tok->text, LEX_MAX_IDENT);
}

static int is_compound_assign(TokenKind k) {
    return k == TOK_PLUS_ASSIGN || k == TOK_MINUS_ASSIGN ||
           k == TOK_STAR_ASSIGN || k == TOK_SLASH_ASSIGN ||
           k == TOK_PERCENT_ASSIGN;
}

static BinOp compound_binop(TokenKind k) {
    switch (k) {
        case TOK_PLUS_ASSIGN:  return OP_PLUS;
        case TOK_MINUS_ASSIGN: return OP_MINUS;
        case TOK_STAR_ASSIGN:  return OP_MUL;
        case TOK_SLASH_ASSIGN: return OP_DIV;
        default:               return OP_MOD; /* TOK_PERCENT_ASSIGN */
    }
}

/* Build the desugared `name = name op 1` for ++/--. */
static Stmt *make_incr_decr(Parser *p, const char *name, TokenKind op_tok) {
    Expr *lhs = ast_var(name);
    check_alloc(p, lhs, NULL, NULL);
    Expr *one = ast_number(1);
    check_alloc(p, one, lhs, NULL);
    Expr *value = ast_binary(op_tok == TOK_INCR ? OP_PLUS : OP_MINUS, lhs, one);
    check_alloc(p, value, lhs, one);
    Stmt *s = stmt_assign(name, value);
    if (!s) { ast_free(value); parser_error(p, "out of memory"); }
    return s;
}

/* Parse a "simple" statement -- assignment (plain, compound, or indexed),
 * increment/decrement, or a bare expression -- WITHOUT consuming any
 * trailing ';'. Shared by ordinary statements and by the init/update
 * clauses of `for`, whose production owns the delimiters. */
static Stmt *parse_simple_stmt(Parser *p) {
    Token tok = p->lex->current_token;

    if (tok.kind == TOK_IDENT) {
        TokenKind pk = lexer_peek(p->lex)->kind;

        /* Plain assignment: IDENT '=' expr. One-token lookahead is enough
         * to distinguish this from an expression statement that merely
         * begins with a variable reference (`==` is a distinct token). */
        if (pk == TOK_ASSIGN) {
            char name[LEX_MAX_IDENT];
            copy_ident(name, &tok);
            lexer_next(p->lex); /* consume IDENT */
            lexer_next(p->lex); /* consume '='   */
            Expr *value = parse_logic_or(p);
            Stmt *s = stmt_assign(name, value);
            if (!s) { ast_free(value); parser_error(p, "out of memory"); }
            return s;
        }

        /* Compound assignment: `x op= e` desugars to `x = x op (e)`, so it
         * reuses the existing LOAD/op/STORE codegen -- no new opcodes. */
        if (is_compound_assign(pk)) {
            char name[LEX_MAX_IDENT];
            copy_ident(name, &tok);
            lexer_next(p->lex); /* consume IDENT */
            lexer_next(p->lex); /* consume op=   */
            Expr *rhs = parse_logic_or(p);
            Expr *lhs = ast_var(name);
            check_alloc(p, lhs, rhs, NULL);
            Expr *value = ast_binary(compound_binop(pk), lhs, rhs);
            check_alloc(p, value, lhs, rhs);
            Stmt *s = stmt_assign(name, value);
            if (!s) { ast_free(value); parser_error(p, "out of memory"); }
            return s;
        }

        /* Postfix increment/decrement: statement-only sugar. */
        if (pk == TOK_INCR || pk == TOK_DECR) {
            char name[LEX_MAX_IDENT];
            copy_ident(name, &tok);
            lexer_next(p->lex); /* consume IDENT   */
            lexer_next(p->lex); /* consume ++ / -- */
            return make_incr_decr(p, name, pk);
        }
    }

    /* Prefix increment/decrement: also statement-only sugar. */
    if (tok.kind == TOK_INCR || tok.kind == TOK_DECR) {
        lexer_next(p->lex);
        if (p->lex->current_token.kind != TOK_IDENT) {
            parser_error(p, tok.kind == TOK_INCR
                            ? "expected identifier after prefix '++'"
                            : "expected identifier after prefix '--'");
        }
        char name[LEX_MAX_IDENT];
        copy_ident(name, &p->lex->current_token);
        lexer_next(p->lex);
        return make_incr_decr(p, name, tok.kind);
    }

    /* Anything else starts an expression. If it turns out to be an array
     * element followed by '=', it's an indexed store: `arr[i] = e`. The
     * subscript may contain an arbitrary expression, so one-token lookahead
     * can't decide this up front; instead the expression parser produces
     * EXPR_INDEX and the '=' is detected afterwards. */
    Expr *e = parse_logic_or(p);
    if (e->kind == EXPR_INDEX && p->lex->current_token.kind == TOK_ASSIGN) {
        lexer_next(p->lex); /* consume '=' */
        Expr *value = parse_logic_or(p);
        char name[LEX_MAX_IDENT];
        strncpy(name, e->u.index.name, LEX_MAX_IDENT - 1);
        name[LEX_MAX_IDENT - 1] = '\0';
        Expr *idx = e->u.index.index;
        e->u.index.index = NULL; /* steal the index, then free the shell */
        ast_free(e);
        Stmt *s = stmt_index_assign(name, idx, value);
        if (!s) { ast_free(idx); ast_free(value); parser_error(p, "out of memory"); }
        return s;
    }
    Stmt *s = stmt_expr(e);
    if (!s) { ast_free(e); parser_error(p, "out of memory"); }
    return s;
}

static Stmt *parse_block(Parser *p) {
    /* assumes current token is '{' */
    lexer_next(p->lex); /* consume '{' */
    StmtList list;
    stmtlist_init(&list);
    while (p->lex->current_token.kind != TOK_RBRACE) {
        if (p->lex->current_token.kind == TOK_EOF) {
            stmtlist_free(&list);
            parser_error(p, "unexpected end of input, expected '}'");
        }
        Stmt *s = parse_stmt(p);
        stmtlist_push(&list, s);
    }
    lexer_next(p->lex); /* consume '}' */
    Stmt *blk = stmt_block(list);
    if (!blk) { stmtlist_free(&list); parser_error(p, "out of memory"); }
    return blk;
}

/* Parse a variable or array declaration after ('int'|'bool') has been seen
 * but not consumed. Does NOT consume the trailing ';' if with_semi is 0
 * (used by for-loop initializers, where the `for` production owns it). */
static Stmt *parse_decl(Parser *p, int with_semi, int allow_array) {
    lexer_next(p->lex); /* consume 'int' / 'bool' */
    if (p->lex->current_token.kind != TOK_IDENT) {
        parser_error(p, "expected identifier after type keyword");
    }
    char name[LEX_MAX_IDENT];
    copy_ident(name, &p->lex->current_token);
    lexer_next(p->lex);

    if (allow_array && p->lex->current_token.kind == TOK_LBRACKET) {
        lexer_next(p->lex);
        Expr *size = parse_logic_or(p);
        if (p->lex->current_token.kind != TOK_RBRACKET) {
            ast_free(size);
            parser_error(p, "expected ']' in array declaration");
        }
        lexer_next(p->lex);
        if (with_semi) expect(p, TOK_SEMI, "expected ';' after array declaration");
        Stmt *s = stmt_arrdecl(name, size);
        if (!s) { ast_free(size); parser_error(p, "out of memory"); }
        return s;
    }

    expect(p, TOK_ASSIGN, allow_array
           ? "expected '=' or '[' in declaration"
           : "expected '=' in variable declaration");
    Expr *init = parse_logic_or(p);
    if (with_semi) expect(p, TOK_SEMI, "expected ';' after declaration");
    Stmt *s = stmt_vardecl(name, init);
    if (!s) { ast_free(init); parser_error(p, "out of memory"); }
    return s;
}

static Stmt *parse_stmt_inner(Parser *p) {
    p->depth = 0; /* reset expression depth per statement */
    Token tok = p->lex->current_token;

    if (tok.kind == TOK_LBRACE) {
        return parse_block(p);
    }

    if (tok.kind == TOK_KW_INT || tok.kind == TOK_KW_BOOL) {
        return parse_decl(p, /*with_semi=*/1, /*allow_array=*/1);
    }

    if (tok.kind == TOK_KW_PRINT) {
        lexer_next(p->lex);
        expect(p, TOK_LPAREN, "expected '(' after 'print'");
        if (p->lex->current_token.kind == TOK_STRING) {
            char text[LEX_MAX_STRING];
            memcpy(text, p->lex->current_token.str_text, LEX_MAX_STRING);
            lexer_next(p->lex);
            expect(p, TOK_RPAREN, "expected ')' after string literal");
            expect(p, TOK_SEMI, "expected ';' after print statement");
            Stmt *s = stmt_print_str(text);
            if (!s) parser_error(p, "out of memory");
            return s;
        }
        Expr *e = parse_logic_or(p);
        if (p->lex->current_token.kind != TOK_RPAREN) {
            ast_free(e);
            parser_error(p, "expected ')' after print argument");
        }
        lexer_next(p->lex);
        expect(p, TOK_SEMI, "expected ';' after print statement");
        Stmt *s = stmt_print(e);
        if (!s) { ast_free(e); parser_error(p, "out of memory"); }
        return s;
    }

    if (tok.kind == TOK_KW_IF) {
        lexer_next(p->lex);
        expect(p, TOK_LPAREN, "expected '(' after 'if'");
        Expr *cond = parse_logic_or(p);
        if (p->lex->current_token.kind != TOK_RPAREN) {
            ast_free(cond);
            parser_error(p, "expected ')' after if condition");
        }
        lexer_next(p->lex);
        Stmt *then_branch = parse_stmt(p);
        Stmt *else_branch = NULL;
        if (p->lex->current_token.kind == TOK_KW_ELSE) {
            lexer_next(p->lex);
            else_branch = parse_stmt(p);
        }
        Stmt *s = stmt_if(cond, then_branch, else_branch);
        if (!s) {
            ast_free(cond); stmt_free(then_branch); stmt_free(else_branch);
            parser_error(p, "out of memory");
        }
        return s;
    }

    if (tok.kind == TOK_KW_WHILE) {
        lexer_next(p->lex);
        expect(p, TOK_LPAREN, "expected '(' after 'while'");
        Expr *cond = parse_logic_or(p);
        if (p->lex->current_token.kind != TOK_RPAREN) {
            ast_free(cond);
            parser_error(p, "expected ')' after while condition");
        }
        lexer_next(p->lex);
        p->loop_depth++;
        Stmt *body = parse_stmt(p);
        p->loop_depth--;
        Stmt *s = stmt_while(cond, body);
        if (!s) { ast_free(cond); stmt_free(body); parser_error(p, "out of memory"); }
        return s;
    }

    if (tok.kind == TOK_KW_FOR) {
        /* for ( init? ; cond? ; update? ) stmt
         * init/update do not consume their own delimiter; the `for`
         * production consumes the ';' after init and the ')' after update. */
        lexer_next(p->lex);
        expect(p, TOK_LPAREN, "expected '(' after 'for'");

        Stmt *init = NULL;
        if (p->lex->current_token.kind != TOK_SEMI) {
            if (p->lex->current_token.kind == TOK_KW_INT ||
                p->lex->current_token.kind == TOK_KW_BOOL) {
                init = parse_decl(p, /*with_semi=*/0, /*allow_array=*/0);
            } else {
                init = parse_simple_stmt(p);
            }
        }
        expect(p, TOK_SEMI, "expected ';' after for-loop initializer");

        Expr *cond = NULL; /* NULL condition == always true */
        if (p->lex->current_token.kind != TOK_SEMI) {
            cond = parse_logic_or(p);
        }
        expect(p, TOK_SEMI, "expected ';' after for-loop condition");

        Stmt *update = NULL;
        if (p->lex->current_token.kind != TOK_RPAREN) {
            update = parse_simple_stmt(p);
        }
        expect(p, TOK_RPAREN, "expected ')' after for-loop clauses");

        p->loop_depth++;
        Stmt *body = parse_stmt(p);
        p->loop_depth--;

        Stmt *s = stmt_for(init, cond, update, body);
        if (!s) {
            stmt_free(init); ast_free(cond); stmt_free(update); stmt_free(body);
            parser_error(p, "out of memory");
        }
        return s;
    }

    /* break/continue outside a loop are rejected here, at parse time, via
     * the loop_depth nesting counter -- rather than at codegen time via a
     * NULL loop context -- because only the parser has the token's
     * line/column; the AST carries no source locations, so a codegen-time
     * diagnostic could not point at the offending statement. */
    if (tok.kind == TOK_KW_BREAK) {
        if (p->loop_depth == 0) {
            parser_error(p, "'break' used outside of a loop");
        }
        lexer_next(p->lex);
        expect(p, TOK_SEMI, "expected ';' after 'break'");
        Stmt *s = stmt_break();
        if (!s) parser_error(p, "out of memory");
        return s;
    }

    if (tok.kind == TOK_KW_CONTINUE) {
        if (p->loop_depth == 0) {
            parser_error(p, "'continue' used outside of a loop");
        }
        lexer_next(p->lex);
        expect(p, TOK_SEMI, "expected ';' after 'continue'");
        Stmt *s = stmt_continue();
        if (!s) parser_error(p, "out of memory");
        return s;
    }

    if (tok.kind == TOK_KW_RETURN) {
        lexer_next(p->lex);
        Expr *value = NULL; /* bare `return;` returns 0 */
        if (p->lex->current_token.kind != TOK_SEMI) {
            value = parse_logic_or(p);
        }
        expect(p, TOK_SEMI, "expected ';' after return statement");
        Stmt *s = stmt_return(value);
        if (!s) { ast_free(value); parser_error(p, "out of memory"); }
        return s;
    }

    /* simple statement (assignment / compound / ++ / -- / indexed store /
     * bare expression), followed by ';' */
    Stmt *s = parse_simple_stmt(p);
    expect(p, TOK_SEMI, "expected ';' after statement");
    return s;
}

static Stmt *parse_stmt(Parser *p) {
    /* Statement nesting (blocks, if/else chains, loop bodies) recurses
     * through parse_stmt; guard it just like expression nesting so deeply
     * nested `{{{...}}}` fails cleanly instead of overflowing the C stack. */
    if (++p->stmt_depth > PARSER_MAX_DEPTH) {
        parser_error(p, "statement nesting too deep");
    }
    Stmt *s = parse_stmt_inner(p);
    p->stmt_depth--;
    return s;
}

/* ---------- top level: function declarations ---------- */

static void parse_funcdecl(Parser *p, FuncDecl *out) {
    if (p->lex->current_token.kind != TOK_KW_INT) {
        parser_error(p, "expected 'int' at top level (program mode allows only "
                        "function declarations; the entry point is 'int main()')");
    }
    lexer_next(p->lex);

    if (p->lex->current_token.kind != TOK_IDENT) {
        parser_error(p, "expected function name after 'int'");
    }
    copy_ident(out->name, &p->lex->current_token);
    lexer_next(p->lex);

    expect(p, TOK_LPAREN, "expected '(' after function name");

    out->param_count = 0;
    if (p->lex->current_token.kind != TOK_RPAREN) {
        for (;;) {
            if (p->lex->current_token.kind != TOK_KW_INT) {
                parser_error(p, "expected 'int' in parameter list");
            }
            lexer_next(p->lex);
            if (p->lex->current_token.kind != TOK_IDENT) {
                parser_error(p, "expected parameter name");
            }
            if (out->param_count >= FUNC_MAX_PARAMS) {
                parser_error(p, "too many parameters (max 16)");
            }
            copy_ident(out->params[out->param_count], &p->lex->current_token);
            out->param_count++;
            lexer_next(p->lex);
            if (p->lex->current_token.kind == TOK_COMMA) {
                lexer_next(p->lex);
                continue;
            }
            break;
        }
    }
    expect(p, TOK_RPAREN, "expected ')' after parameter list");

    if (p->lex->current_token.kind != TOK_LBRACE) {
        parser_error(p, "expected '{' to begin function body");
    }
    out->body = parse_block(p);
}

FuncList parse_program(Parser *p) {
    FuncList fl;
    funclist_init(&fl);
    while (p->lex->current_token.kind != TOK_EOF) {
        FuncDecl fd;
        memset(&fd, 0, sizeof(fd));
        parse_funcdecl(p, &fd);
        funclist_push(&fl, &fd);
    }
    return fl;
}
