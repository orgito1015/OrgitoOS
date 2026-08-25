/* OrgitoCompiler codegen - ported from OrionCC. See oc_platform.h for the
 * freestanding-kernel substitutions (malloc/free/realloc, exit->OC_FAIL). */
#include "oc_platform.h"
#include "codegen.h"

/* ---------- bytecode buffer ---------- */

void bc_init(Bytecode *bc) {
    bc->data = NULL;
    bc->count = 0;
    bc->capacity = 0;
    bc->strs = NULL;
    bc->str_count = 0;
    bc->str_capacity = 0;
    bc->funcs = NULL;
    bc->func_count = 0;
    bc->func_capacity = 0;
}

void bc_emit(Bytecode *bc, int value) {
    if (bc->count >= bc->capacity) {
        size_t old_cap = bc->capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 16;
        int *new_data = (int *)oc_realloc(bc->data, old_cap * sizeof(int),
                                          new_cap * sizeof(int));
        if (!new_data) {
            bc_free(bc);
            OC_FAIL("Codegen error: out of memory\n");
        }
        bc->data = new_data;
        bc->capacity = new_cap;
    }
    bc->data[bc->count++] = value;
}

int bc_add_string(Bytecode *bc, const char *text) {
    if (bc->str_count >= bc->str_capacity) {
        size_t old_cap = bc->str_capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 8;
        char **nd = (char **)oc_realloc(bc->strs, old_cap * sizeof(char *),
                                        new_cap * sizeof(char *));
        if (!nd) {
            OC_FAIL("Codegen error: out of memory\n");
        }
        bc->strs = nd;
        bc->str_capacity = new_cap;
    }
    size_t n = strlen(text);
    char *copy = (char *)oc_malloc(n + 1);
    if (!copy) {
        OC_FAIL("Codegen error: out of memory\n");
    }
    memcpy(copy, text, n + 1);
    bc->strs[bc->str_count] = copy;
    return (int)bc->str_count++;
}

int bc_add_func(Bytecode *bc, int arity) {
    if (bc->func_count >= bc->func_capacity) {
        size_t old_cap = bc->func_capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 8;
        BcFunc *nd = (BcFunc *)oc_realloc(bc->funcs, old_cap * sizeof(BcFunc),
                                          new_cap * sizeof(BcFunc));
        if (!nd) {
            OC_FAIL("Codegen error: out of memory\n");
        }
        bc->funcs = nd;
        bc->func_capacity = new_cap;
    }
    bc->funcs[bc->func_count].entry = -1;
    bc->funcs[bc->func_count].arity = arity;
    bc->funcs[bc->func_count].nlocals = 0;
    return (int)bc->func_count++;
}

void bc_free(Bytecode *bc) {
    oc_free(bc->data);
    for (size_t i = 0; i < bc->str_count; i++) oc_free(bc->strs[i]);
    oc_free(bc->strs);
    oc_free(bc->funcs);
    bc->data = NULL;
    bc->count = 0;
    bc->capacity = 0;
    bc->strs = NULL;
    bc->str_count = 0;
    bc->str_capacity = 0;
    bc->funcs = NULL;
    bc->func_count = 0;
    bc->func_capacity = 0;
}

/* ---------- scoped symbol table ----------
 *
 * One symbol table per function. Symbols record the scope depth at which
 * they were declared:
 *
 *   - symtab_find() searches from the MOST RECENTLY declared symbol
 *     backwards, so an inner declaration shadows any outer one with the
 *     same name.
 *   - symtab_declare() rejects a redeclaration only within the SAME scope
 *     depth; shadowing an outer scope's variable is allowed.
 *   - symtab_enter_scope()/symtab_leave_scope() bracket blocks (and a
 *     for-loop's own init scope). Leaving a scope pops its symbols AND
 *     rewinds next_slot, so sibling scopes REUSE frame slots. Reuse is safe
 *     because every scalar declaration stores before any read, and array
 *     declarations emit an explicit BC_ZERO over their slot range (so a
 *     reused region can't leak a dead sibling's values). Because slots are
 *     reused, deep block nesting doesn't grow the frame and the old flat
 *     VAR_MAX=256 didn't need raising; the per-frame budget is
 *     FRAME_SLOTS_MAX with max_slots tracking the high-water mark that
 *     becomes the frame size.
 */

#define SYM_MAX 1024

typedef struct {
    char name[LEX_MAX_IDENT];
    int  slot;      /* frame-relative base offset            */
    int  scope;     /* scope depth at declaration            */
    int  is_array;
    int  size;      /* slots occupied (1 for a scalar)       */
} Symbol;

typedef struct {
    Symbol syms[SYM_MAX];
    size_t count;
    int    scope;      /* current scope depth                */
    int    next_slot;  /* next free frame-relative slot      */
    int    max_slots;  /* high-water mark == frame size      */
} SymbolTable;

static void symtab_init(SymbolTable *st) {
    st->count = 0;
    st->scope = 0;
    st->next_slot = 0;
    st->max_slots = 0;
}

static void symtab_enter_scope(SymbolTable *st) {
    st->scope++;
}

static void symtab_leave_scope(SymbolTable *st) {
    while (st->count > 0 && st->syms[st->count - 1].scope == st->scope) {
        st->count--;
        st->next_slot = st->syms[st->count].slot; /* rewind: slot reuse */
    }
    st->scope--;
}

/* Return the innermost symbol with this name, or NULL if undeclared. */
static const Symbol *symtab_find(const SymbolTable *st, const char *name) {
    for (size_t i = st->count; i > 0; i--) {
        if (strcmp(st->syms[i - 1].name, name) == 0) return &st->syms[i - 1];
    }
    return NULL;
}

/* Declare a name in the current scope; errors on redeclaration at the SAME
 * scope depth only. Returns the assigned frame-relative slot. */
static int symtab_declare(SymbolTable *st, const char *name, int is_array, int size) {
    for (size_t i = st->count; i > 0; i--) {
        const Symbol *s = &st->syms[i - 1];
        if (s->scope < st->scope) break; /* older scopes: shadowing is fine */
        if (strcmp(s->name, name) == 0) {
            OC_FAIL("Codegen error: variable '%s' redeclared in the same scope\n",
                    name);
        }
    }
    if (st->count >= SYM_MAX) {
        OC_FAIL("Codegen error: too many variables in function (max %d)\n",
                SYM_MAX);
    }
    if (size < 1 || st->next_slot > FRAME_SLOTS_MAX - size) {
        OC_FAIL("Codegen error: function frame too large declaring '%s' (max %d slots)\n",
                name, FRAME_SLOTS_MAX);
    }
    Symbol *sym = &st->syms[st->count++];
    strncpy(sym->name, name, LEX_MAX_IDENT - 1);
    sym->name[LEX_MAX_IDENT - 1] = '\0';
    sym->slot = st->next_slot;
    sym->scope = st->scope;
    sym->is_array = is_array;
    sym->size = size;
    st->next_slot += size;
    if (st->next_slot > st->max_slots) st->max_slots = st->next_slot;
    return sym->slot;
}

static const Symbol *symtab_require(const SymbolTable *st, const char *name) {
    const Symbol *sym = symtab_find(st, name);
    if (!sym) {
        OC_FAIL("Codegen error: use of undeclared variable '%s'\n", name);
    }
    return sym;
}

static const Symbol *require_scalar(const SymbolTable *st, const char *name) {
    const Symbol *sym = symtab_require(st, name);
    if (sym->is_array) {
        OC_FAIL("Codegen error: array '%s' used without a subscript\n", name);
    }
    return sym;
}

static const Symbol *require_array(const SymbolTable *st, const char *name) {
    const Symbol *sym = symtab_require(st, name);
    if (!sym->is_array) {
        OC_FAIL("Codegen error: subscript applied to non-array variable '%s'\n",
                name);
    }
    return sym;
}

/* ---------- loop context (break/continue backpatching) ----------
 *
 * Each while/for gets a fresh LoopCtx. `break` and `continue` emit BC_JMP
 * with a placeholder target and record the operand's offset; when the loop
 * finishes codegen and its exit / continue-target positions are known, all
 * pending offsets are backpatched (same pattern as if/else patching, but
 * with a growable list because a loop may contain any number of them).
 * `continue` targets the update clause for `for` and the condition (loop
 * top) for `while`.
 */

typedef struct {
    size_t *items;
    size_t count;
    size_t capacity;
} PatchList;

static void patchlist_init(PatchList *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

static void patchlist_push(PatchList *l, size_t offset) {
    if (l->count >= l->capacity) {
        size_t old_cap = l->capacity;
        size_t new_cap = old_cap ? old_cap * 2 : 4;
        size_t *nd = (size_t *)oc_realloc(l->items, old_cap * sizeof(size_t),
                                          new_cap * sizeof(size_t));
        if (!nd) {
            OC_FAIL("Codegen error: out of memory\n");
        }
        l->items = nd;
        l->capacity = new_cap;
    }
    l->items[l->count++] = offset;
}

static void patchlist_apply(Bytecode *bc, const PatchList *l, size_t target) {
    for (size_t i = 0; i < l->count; i++) {
        bc->data[l->items[i]] = (int)target;
    }
}

static void patchlist_free(PatchList *l) {
    oc_free(l->items);
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

typedef struct {
    PatchList breaks;    /* BC_JMP operands to patch to the loop's exit   */
    PatchList continues; /* BC_JMP operands to patch to the continue point */
} LoopCtx;

/* ---------- codegen context ---------- */

typedef struct {
    Bytecode       *bc;
    SymbolTable     st;
    const FuncDecl *decls;  /* function signatures; index == BC_CALL index */
    size_t          ndecls;
} Codegen;

static int find_func(const Codegen *cg, const char *name) {
    for (size_t i = 0; i < cg->ndecls; i++) {
        if (strcmp(cg->decls[i].name, name) == 0) return (int)i;
    }
    return -1;
}

/* ---------- compile-time constant evaluation (array sizes) ---------- */

static int const_eval(const Expr *e, int *ok) {
    if (!*ok) return 0;
    switch (e->kind) {
        case EXPR_NUMBER:
            return e->u.number.value;
        case EXPR_UNARY: {
            int v = const_eval(e->u.unary.expr, ok);
            if (!*ok) return 0;
            return e->u.unary.op == UOP_NEG ? -v : (v == 0 ? 1 : 0);
        }
        case EXPR_BINARY: {
            int a = const_eval(e->u.binary.left, ok);
            int b = const_eval(e->u.binary.right, ok);
            if (!*ok) return 0;
            switch (e->u.binary.op) {
                case OP_PLUS:  return a + b;
                case OP_MINUS: return a - b;
                case OP_MUL:   return a * b;
                case OP_DIV:   if (b == 0) { *ok = 0; return 0; } return a / b;
                case OP_MOD:   if (b == 0) { *ok = 0; return 0; } return a % b;
                case OP_LT:    return a <  b;
                case OP_GT:    return a >  b;
                case OP_LE:    return a <= b;
                case OP_GE:    return a >= b;
                case OP_EQ:    return a == b;
                case OP_NE:    return a != b;
                case OP_AND:   return (a != 0) && (b != 0);
                case OP_OR:    return (a != 0) || (b != 0);
            }
            *ok = 0;
            return 0;
        }
        case EXPR_VAR:
        case EXPR_CALL:
        case EXPR_INDEX:
            *ok = 0; /* not a compile-time constant */
            return 0;
    }
    *ok = 0;
    return 0;
}

/* ---------- expressions ---------- */

static void codegen_logical(Codegen *cg, const Expr *expr);

/* Emit code that pushes the value of `expr` onto the stack. Logical && / ||
 * are dispatched to a short-circuit jump-based implementation. */
static void codegen_value(Codegen *cg, const Expr *expr) {
    Bytecode *bc = cg->bc;

    if (expr->kind == EXPR_BINARY &&
        (expr->u.binary.op == OP_AND || expr->u.binary.op == OP_OR)) {
        codegen_logical(cg, expr);
        return;
    }
    switch (expr->kind) {
        case EXPR_NUMBER:
            bc_emit(bc, BC_PUSH_CONST);
            bc_emit(bc, expr->u.number.value);
            break;
        case EXPR_VAR: {
            const Symbol *sym = require_scalar(&cg->st, expr->u.var.name);
            bc_emit(bc, BC_LOAD);
            bc_emit(bc, sym->slot);
            break;
        }
        case EXPR_INDEX: {
            const Symbol *sym = require_array(&cg->st, expr->u.index.name);
            int slot = sym->slot, size = sym->size;
            codegen_value(cg, expr->u.index.index);
            bc_emit(bc, BC_LOAD_IDX);
            bc_emit(bc, slot);
            bc_emit(bc, size);
            break;
        }
        case EXPR_CALL: {
            int fi = find_func(cg, expr->u.call.name);
            if (fi < 0) {
                OC_FAIL("Codegen error: call to undefined function '%s'\n",
                        expr->u.call.name);
            }
            size_t arity = cg->decls[fi].param_count;
            if (expr->u.call.args.count != arity) {
                OC_FAIL("Codegen error: function '%s' expects %zu argument%s, got %zu\n",
                        expr->u.call.name, arity, arity == 1 ? "" : "s",
                        expr->u.call.args.count);
            }
            /* Arguments left to right; BC_CALL pops them into the frame. */
            for (size_t i = 0; i < expr->u.call.args.count; i++) {
                codegen_value(cg, expr->u.call.args.items[i]);
            }
            bc_emit(bc, BC_CALL);
            bc_emit(bc, fi);
            break;
        }
        case EXPR_UNARY:
            codegen_value(cg, expr->u.unary.expr);
            bc_emit(bc, expr->u.unary.op == UOP_NEG ? BC_NEG : BC_NOT);
            break;
        case EXPR_BINARY:
            codegen_value(cg, expr->u.binary.left);
            codegen_value(cg, expr->u.binary.right);
            switch (expr->u.binary.op) {
                case OP_PLUS:  bc_emit(bc, BC_ADD); break;
                case OP_MINUS: bc_emit(bc, BC_SUB); break;
                case OP_MUL:   bc_emit(bc, BC_MUL); break;
                case OP_DIV:   bc_emit(bc, BC_DIV); break;
                case OP_MOD:   bc_emit(bc, BC_MOD); break;
                case OP_LT:    bc_emit(bc, BC_LT);  break;
                case OP_GT:    bc_emit(bc, BC_GT);  break;
                case OP_LE:    bc_emit(bc, BC_LE);  break;
                case OP_GE:    bc_emit(bc, BC_GE);  break;
                case OP_EQ:    bc_emit(bc, BC_EQ);  break;
                case OP_NE:    bc_emit(bc, BC_NE);  break;
                case OP_AND:
                case OP_OR:
                    /* handled by codegen_logical via the guard above */
                    break;
            }
            break;
    }
}

/* Short-circuit logical operators producing 0/1.
 *
 * a && b:
 *   <a>
 *   JZ  Lfalse        ; if a==0, result is 0
 *   <b>
 *   NOT NOT           ; normalize b to 0/1
 *   JMP Lend
 * Lfalse: PUSH 0
 * Lend:
 *
 * a || b: see inline comment below.
 */
static void codegen_logical(Codegen *cg, const Expr *expr) {
    Bytecode *bc = cg->bc;
    if (expr->u.binary.op == OP_AND) {
        codegen_value(cg, expr->u.binary.left);
        bc_emit(bc, BC_JZ);
        size_t patch_false = bc->count; bc_emit(bc, 0);
        codegen_value(cg, expr->u.binary.right);
        /* normalize right to 0/1 */
        bc_emit(bc, BC_NOT);
        bc_emit(bc, BC_NOT);
        bc_emit(bc, BC_JMP);
        size_t patch_end = bc->count; bc_emit(bc, 0);
        /* Lfalse: */
        bc->data[patch_false] = (int)bc->count;
        bc_emit(bc, BC_PUSH_CONST);
        bc_emit(bc, 0);
        /* Lend: */
        bc->data[patch_end] = (int)bc->count;
    } else { /* OP_OR */
        /* a || b:
         *   <a>
         *   JZ Ltryb        ; a==0 -> evaluate b
         *   PUSH 1          ; a truthy -> 1
         *   JMP Lend
         * Ltryb: <b>
         *   NOT NOT         ; normalize
         * Lend:
         */
        codegen_value(cg, expr->u.binary.left);
        bc_emit(bc, BC_JZ);
        size_t patch_tryb = bc->count; bc_emit(bc, 0);
        bc_emit(bc, BC_PUSH_CONST);
        bc_emit(bc, 1);
        bc_emit(bc, BC_JMP);
        size_t patch_end = bc->count; bc_emit(bc, 0);
        /* Ltryb: */
        bc->data[patch_tryb] = (int)bc->count;
        codegen_value(cg, expr->u.binary.right);
        bc_emit(bc, BC_NOT);
        bc_emit(bc, BC_NOT);
        /* Lend: */
        bc->data[patch_end] = (int)bc->count;
    }
}

/* ---------- statements ---------- */

/* `loop` is NULL outside any loop; while/for install their own LoopCtx and
 * pass it through their body so break/continue bind to the INNERMOST loop.
 * (The parser already rejects break/continue outside any loop, so the NULL
 * checks below are defensive.) */
static void codegen_stmt(Codegen *cg, const Stmt *s, LoopCtx *loop) {
    Bytecode *bc = cg->bc;

    switch (s->kind) {
        case STMT_EXPR:
            codegen_value(cg, s->u.expr.expr);
            bc_emit(bc, BC_POP);
            break;
        case STMT_VARDECL: {
            codegen_value(cg, s->u.vardecl.init);
            int slot = symtab_declare(&cg->st, s->u.vardecl.name, 0, 1);
            bc_emit(bc, BC_STORE);
            bc_emit(bc, slot);
            break;
        }
        case STMT_ARRDECL: {
            int ok = 1;
            int n = const_eval(s->u.arrdecl.size, &ok);
            if (!ok) {
                OC_FAIL("Codegen error: size of array '%s' must be a compile-time constant expression\n",
                        s->u.arrdecl.name);
            }
            if (n <= 0 || n > FRAME_SLOTS_MAX) {
                OC_FAIL("Codegen error: size of array '%s' must be between 1 and %d (got %d)\n",
                        s->u.arrdecl.name, FRAME_SLOTS_MAX, n);
            }
            int slot = symtab_declare(&cg->st, s->u.arrdecl.name, 1, (int)n);
            /* Explicit zeroing: frame slots are reused across sibling scopes,
             * so without this an array could observe a dead sibling's values. */
            bc_emit(bc, BC_ZERO);
            bc_emit(bc, slot);
            bc_emit(bc, (int)n);
            break;
        }
        case STMT_ASSIGN: {
            codegen_value(cg, s->u.assign.value);
            const Symbol *sym = require_scalar(&cg->st, s->u.assign.name);
            bc_emit(bc, BC_STORE);
            bc_emit(bc, sym->slot);
            break;
        }
        case STMT_INDEX_ASSIGN: {
            const Symbol *sym = require_array(&cg->st, s->u.index_assign.name);
            int slot = sym->slot, size = sym->size;
            codegen_value(cg, s->u.index_assign.value);
            codegen_value(cg, s->u.index_assign.index);
            bc_emit(bc, BC_STORE_IDX);
            bc_emit(bc, slot);
            bc_emit(bc, size);
            break;
        }
        case STMT_PRINT:
            codegen_value(cg, s->u.print.expr);
            bc_emit(bc, BC_PRINT);
            break;
        case STMT_PRINT_STR: {
            int idx = bc_add_string(bc, s->u.print_str.text);
            bc_emit(bc, BC_PRINT_STR);
            bc_emit(bc, idx);
            break;
        }
        case STMT_IF: {
            codegen_value(cg, s->u.if_stmt.cond);
            bc_emit(bc, BC_JZ);
            size_t patch_else = bc->count; bc_emit(bc, 0);
            codegen_stmt(cg, s->u.if_stmt.then_branch, loop);
            if (s->u.if_stmt.else_branch) {
                bc_emit(bc, BC_JMP);
                size_t patch_end = bc->count; bc_emit(bc, 0);
                bc->data[patch_else] = (int)bc->count;
                codegen_stmt(cg, s->u.if_stmt.else_branch, loop);
                bc->data[patch_end] = (int)bc->count;
            } else {
                bc->data[patch_else] = (int)bc->count;
            }
            break;
        }
        case STMT_WHILE: {
            LoopCtx ctx;
            patchlist_init(&ctx.breaks);
            patchlist_init(&ctx.continues);

            size_t loop_top = bc->count;
            codegen_value(cg, s->u.while_stmt.cond);
            bc_emit(bc, BC_JZ);
            size_t patch_exit = bc->count; bc_emit(bc, 0);
            codegen_stmt(cg, s->u.while_stmt.body, &ctx);
            bc_emit(bc, BC_JMP);
            bc_emit(bc, (int)loop_top);
            size_t exit_ip = bc->count;
            bc->data[patch_exit] = (int)exit_ip;
            /* continue re-tests the condition; break leaves the loop */
            patchlist_apply(bc, &ctx.continues, loop_top);
            patchlist_apply(bc, &ctx.breaks, exit_ip);
            patchlist_free(&ctx.continues);
            patchlist_free(&ctx.breaks);
            break;
        }
        case STMT_FOR: {
            /* Desugared here to the same JZ/JMP shape as while:
             *   <init>
             * Ltop:   <cond or nothing>   JZ Lexit (only if cond present)
             *         <body>
             * Lcont:  <update>
             *         JMP Ltop
             * Lexit:
             * The init variable lives in the for statement's own scope. */
            LoopCtx ctx;
            patchlist_init(&ctx.breaks);
            patchlist_init(&ctx.continues);

            symtab_enter_scope(&cg->st);
            if (s->u.for_stmt.init) {
                codegen_stmt(cg, s->u.for_stmt.init, NULL);
            }
            size_t loop_top = bc->count;
            size_t patch_exit = 0;
            int has_cond = s->u.for_stmt.cond != NULL;
            if (has_cond) {
                codegen_value(cg, s->u.for_stmt.cond);
                bc_emit(bc, BC_JZ);
                patch_exit = bc->count; bc_emit(bc, 0);
            } /* omitted condition defaults to true: fall through into body */
            codegen_stmt(cg, s->u.for_stmt.body, &ctx);
            size_t update_ip = bc->count; /* `continue` lands here */
            if (s->u.for_stmt.update) {
                codegen_stmt(cg, s->u.for_stmt.update, NULL);
            }
            bc_emit(bc, BC_JMP);
            bc_emit(bc, (int)loop_top);
            size_t exit_ip = bc->count;
            if (has_cond) bc->data[patch_exit] = (int)exit_ip;
            patchlist_apply(bc, &ctx.continues, update_ip);
            patchlist_apply(bc, &ctx.breaks, exit_ip);
            patchlist_free(&ctx.continues);
            patchlist_free(&ctx.breaks);
            symtab_leave_scope(&cg->st);
            break;
        }
        case STMT_BREAK: {
            if (!loop) {
                OC_FAIL("Codegen error: 'break' outside of a loop\n");
            }
            bc_emit(bc, BC_JMP);
            patchlist_push(&loop->breaks, bc->count);
            bc_emit(bc, 0); /* patched to the loop exit */
            break;
        }
        case STMT_CONTINUE: {
            if (!loop) {
                OC_FAIL("Codegen error: 'continue' outside of a loop\n");
            }
            bc_emit(bc, BC_JMP);
            patchlist_push(&loop->continues, bc->count);
            bc_emit(bc, 0); /* patched to the update point (for) / loop top (while) */
            break;
        }
        case STMT_RETURN:
            if (s->u.ret.value) {
                codegen_value(cg, s->u.ret.value);
            } else {
                bc_emit(bc, BC_PUSH_CONST);
                bc_emit(bc, 0); /* bare `return;` returns 0 */
            }
            bc_emit(bc, BC_RET);
            break;
        case STMT_BLOCK:
            symtab_enter_scope(&cg->st);
            for (size_t i = 0; i < s->u.block.list.count; i++) {
                codegen_stmt(cg, s->u.block.list.items[i], loop);
            }
            symtab_leave_scope(&cg->st);
            break;
    }
}

/* ---------- program (functions) ---------- */

void codegen_program(Bytecode *bc, const FuncList *program) {
    /* Pass 1: register every signature before generating any body, so
     * functions can call each other regardless of declaration order and
     * recursion resolves through the function table. */
    int main_idx = -1;
    for (size_t i = 0; i < program->count; i++) {
        const FuncDecl *f = &program->items[i];
        for (size_t j = 0; j < i; j++) {
            if (strcmp(program->items[j].name, f->name) == 0) {
                OC_FAIL("Codegen error: function '%s' redeclared\n", f->name);
            }
        }
        if (strcmp(f->name, "main") == 0) {
            if (f->param_count != 0) {
                OC_FAIL("Codegen error: 'main' must take no parameters\n");
            }
            main_idx = (int)i;
        }
        bc_add_func(bc, (int)f->param_count);
    }
    if (main_idx < 0) {
        OC_FAIL("Codegen error: program must define 'int main()'\n");
    }

    /* Prologue: call main, halt when it returns. main's return value is
     * left on the stack for BC_HALT (the driver ignores it in program mode). */
    bc_emit(bc, BC_CALL);
    bc_emit(bc, main_idx);
    bc_emit(bc, BC_HALT);

    /* Pass 2: generate each body, filling in entry offsets and frame sizes. */
    Codegen cg;
    cg.bc = bc;
    cg.decls = program->items;
    cg.ndecls = program->count;

    for (size_t i = 0; i < program->count; i++) {
        const FuncDecl *f = &program->items[i];
        symtab_init(&cg.st);
        /* Parameters are the frame's first slots (scope depth 0). The body's
         * statements are generated at the SAME scope depth, so declaring a
         * variable with a parameter's name at the top level of the body is a
         * redeclaration error, matching C. */
        for (size_t pi = 0; pi < f->param_count; pi++) {
            (void)symtab_declare(&cg.st, f->params[pi], 0, 1);
        }
        bc->funcs[i].entry = (int)bc->count;
        const StmtList *body = &f->body->u.block.list;
        for (size_t si = 0; si < body->count; si++) {
            codegen_stmt(&cg, body->items[si], NULL);
        }
        /* Implicit `return 0;` for functions that fall off the end. */
        bc_emit(bc, BC_PUSH_CONST);
        bc_emit(bc, 0);
        bc_emit(bc, BC_RET);
        bc->funcs[i].nlocals = cg.st.max_slots;
    }
}

/* ---------- legacy single-expression path ---------- */

void codegen_expr(Bytecode *bc, Expr *expr) {
    Codegen cg;
    cg.bc = bc;
    cg.decls = NULL;   /* expression mode has no functions: */
    cg.ndecls = 0;     /* any EXPR_CALL is an undefined-function error */
    symtab_init(&cg.st);
    codegen_value(&cg, expr);
}

void codegen_finish(Bytecode *bc) {
    bc_emit(bc, BC_HALT);
}
