/* OrgitoCompiler AST - ported from OrionCC. See oc_platform.h for the
 * freestanding-kernel substitutions (malloc/free/realloc, exit->OC_FAIL). */
#include "oc_platform.h"
#include "ast.h"

static void oom(void) {
    OC_FAIL("Fatal error: out of memory\n");
}

static void copy_name(char *dst, const char *src) {
    strncpy(dst, src, LEX_MAX_IDENT - 1);
    dst[LEX_MAX_IDENT - 1] = '\0';
}

/* ---------- Expression list ---------- */

void exprlist_init(ExprList *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

void exprlist_push(ExprList *l, Expr *e) {
    if (l->count >= l->capacity) {
        size_t old_cap = l->capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 4;
        Expr **nd = (Expr **)oc_realloc(l->items, old_cap * sizeof(Expr *),
                                        new_cap * sizeof(Expr *));
        if (!nd) oom();
        l->items = nd;
        l->capacity = new_cap;
    }
    l->items[l->count++] = e;
}

void exprlist_free(ExprList *l) {
    for (size_t i = 0; i < l->count; i++) {
        ast_free(l->items[i]);
    }
    oc_free(l->items);
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

/* ---------- Expressions ---------- */

Expr *ast_number(int value) {
    Expr *e = (Expr *)oc_malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_NUMBER;
    e->u.number.value = value;
    return e;
}

Expr *ast_var(const char *name) {
    Expr *e = (Expr *)oc_malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_VAR;
    copy_name(e->u.var.name, name);
    return e;
}

Expr *ast_unary(UnOp op, Expr *expr) {
    Expr *e = (Expr *)oc_malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_UNARY;
    e->u.unary.op = op;
    e->u.unary.expr = expr;
    return e;
}

Expr *ast_binary(BinOp op, Expr *left, Expr *right) {
    Expr *e = (Expr *)oc_malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_BINARY;
    e->u.binary.op = op;
    e->u.binary.left = left;
    e->u.binary.right = right;
    return e;
}

Expr *ast_call(const char *name, ExprList args) {
    Expr *e = (Expr *)oc_malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_CALL;
    copy_name(e->u.call.name, name);
    e->u.call.args = args;
    return e;
}

Expr *ast_index(const char *name, Expr *index) {
    Expr *e = (Expr *)oc_malloc(sizeof(Expr));
    if (!e) return NULL;
    e->kind = EXPR_INDEX;
    copy_name(e->u.index.name, name);
    e->u.index.index = index;
    return e;
}

void ast_free(Expr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case EXPR_NUMBER:
        case EXPR_VAR:
            break;
        case EXPR_UNARY:
            ast_free(expr->u.unary.expr);
            break;
        case EXPR_BINARY:
            ast_free(expr->u.binary.left);
            ast_free(expr->u.binary.right);
            break;
        case EXPR_CALL:
            exprlist_free(&expr->u.call.args);
            break;
        case EXPR_INDEX:
            ast_free(expr->u.index.index);
            break;
    }
    oc_free(expr);
}

/* ---------- Statement list ---------- */

void stmtlist_init(StmtList *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

void stmtlist_push(StmtList *l, Stmt *s) {
    if (l->count >= l->capacity) {
        size_t old_cap = l->capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 8;
        Stmt **nd = (Stmt **)oc_realloc(l->items, old_cap * sizeof(Stmt *),
                                        new_cap * sizeof(Stmt *));
        if (!nd) oom();
        l->items = nd;
        l->capacity = new_cap;
    }
    l->items[l->count++] = s;
}

void stmtlist_free(StmtList *l) {
    for (size_t i = 0; i < l->count; i++) {
        stmt_free(l->items[i]);
    }
    oc_free(l->items);
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

/* ---------- Statements ---------- */

static Stmt *stmt_alloc(StmtKind kind) {
    Stmt *s = (Stmt *)oc_malloc(sizeof(Stmt));
    if (!s) return NULL;
    s->kind = kind;
    return s;
}

Stmt *stmt_expr(Expr *e) {
    Stmt *s = stmt_alloc(STMT_EXPR);
    if (!s) return NULL;
    s->u.expr.expr = e;
    return s;
}

Stmt *stmt_vardecl(const char *name, Expr *init) {
    Stmt *s = stmt_alloc(STMT_VARDECL);
    if (!s) return NULL;
    copy_name(s->u.vardecl.name, name);
    s->u.vardecl.init = init;
    return s;
}

Stmt *stmt_arrdecl(const char *name, Expr *size) {
    Stmt *s = stmt_alloc(STMT_ARRDECL);
    if (!s) return NULL;
    copy_name(s->u.arrdecl.name, name);
    s->u.arrdecl.size = size;
    return s;
}

Stmt *stmt_assign(const char *name, Expr *value) {
    Stmt *s = stmt_alloc(STMT_ASSIGN);
    if (!s) return NULL;
    copy_name(s->u.assign.name, name);
    s->u.assign.value = value;
    return s;
}

Stmt *stmt_index_assign(const char *name, Expr *index, Expr *value) {
    Stmt *s = stmt_alloc(STMT_INDEX_ASSIGN);
    if (!s) return NULL;
    copy_name(s->u.index_assign.name, name);
    s->u.index_assign.index = index;
    s->u.index_assign.value = value;
    return s;
}

Stmt *stmt_print(Expr *e) {
    Stmt *s = stmt_alloc(STMT_PRINT);
    if (!s) return NULL;
    s->u.print.expr = e;
    return s;
}

Stmt *stmt_print_str(const char *text) {
    Stmt *s = stmt_alloc(STMT_PRINT_STR);
    if (!s) return NULL;
    size_t n = strlen(text);
    char *copy = (char *)oc_malloc(n + 1);
    if (!copy) { oc_free(s); return NULL; }
    memcpy(copy, text, n + 1);
    s->u.print_str.text = copy;
    return s;
}

Stmt *stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch) {
    Stmt *s = stmt_alloc(STMT_IF);
    if (!s) return NULL;
    s->u.if_stmt.cond = cond;
    s->u.if_stmt.then_branch = then_branch;
    s->u.if_stmt.else_branch = else_branch;
    return s;
}

Stmt *stmt_while(Expr *cond, Stmt *body) {
    Stmt *s = stmt_alloc(STMT_WHILE);
    if (!s) return NULL;
    s->u.while_stmt.cond = cond;
    s->u.while_stmt.body = body;
    return s;
}

Stmt *stmt_for(Stmt *init, Expr *cond, Stmt *update, Stmt *body) {
    Stmt *s = stmt_alloc(STMT_FOR);
    if (!s) return NULL;
    s->u.for_stmt.init = init;
    s->u.for_stmt.cond = cond;
    s->u.for_stmt.update = update;
    s->u.for_stmt.body = body;
    return s;
}

Stmt *stmt_break(void) {
    return stmt_alloc(STMT_BREAK);
}

Stmt *stmt_continue(void) {
    return stmt_alloc(STMT_CONTINUE);
}

Stmt *stmt_return(Expr *value) {
    Stmt *s = stmt_alloc(STMT_RETURN);
    if (!s) return NULL;
    s->u.ret.value = value;
    return s;
}

Stmt *stmt_block(StmtList list) {
    Stmt *s = stmt_alloc(STMT_BLOCK);
    if (!s) return NULL;
    s->u.block.list = list;
    return s;
}

void stmt_free(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_EXPR:         ast_free(s->u.expr.expr); break;
        case STMT_VARDECL:      ast_free(s->u.vardecl.init); break;
        case STMT_ARRDECL:      ast_free(s->u.arrdecl.size); break;
        case STMT_ASSIGN:       ast_free(s->u.assign.value); break;
        case STMT_INDEX_ASSIGN:
            ast_free(s->u.index_assign.index);
            ast_free(s->u.index_assign.value);
            break;
        case STMT_PRINT:        ast_free(s->u.print.expr); break;
        case STMT_PRINT_STR:    oc_free(s->u.print_str.text); break;
        case STMT_IF:
            ast_free(s->u.if_stmt.cond);
            stmt_free(s->u.if_stmt.then_branch);
            stmt_free(s->u.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            ast_free(s->u.while_stmt.cond);
            stmt_free(s->u.while_stmt.body);
            break;
        case STMT_FOR:
            stmt_free(s->u.for_stmt.init);
            ast_free(s->u.for_stmt.cond);
            stmt_free(s->u.for_stmt.update);
            stmt_free(s->u.for_stmt.body);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE:
            break;
        case STMT_RETURN:
            ast_free(s->u.ret.value);
            break;
        case STMT_BLOCK:
            stmtlist_free(&s->u.block.list);
            break;
    }
    oc_free(s);
}

/* ---------- Function list ---------- */

void funclist_init(FuncList *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

void funclist_push(FuncList *l, const FuncDecl *f) {
    if (l->count >= l->capacity) {
        size_t old_cap = l->capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 4;
        FuncDecl *nd = (FuncDecl *)oc_realloc(l->items, old_cap * sizeof(FuncDecl),
                                              new_cap * sizeof(FuncDecl));
        if (!nd) oom();
        l->items = nd;
        l->capacity = new_cap;
    }
    l->items[l->count++] = *f;
}

void funclist_free(FuncList *l) {
    for (size_t i = 0; i < l->count; i++) {
        stmt_free(l->items[i].body);
    }
    oc_free(l->items);
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}
