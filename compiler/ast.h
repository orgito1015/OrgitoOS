#ifndef AST_H
#define AST_H

#include <stddef.h>
#include "lexer.h" /* for LEX_MAX_IDENT */

/* ---------- Expressions ---------- */

typedef enum {
    EXPR_NUMBER,
    EXPR_VAR,     /* scalar identifier reference          */
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_CALL,    /* name '(' args? ')'                   */
    EXPR_INDEX    /* name '[' index ']' as an rvalue      */
} ExprKind;

typedef enum {
    OP_PLUS,
    OP_MINUS,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    /* comparison */
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_EQ,
    OP_NE,
    /* logical */
    OP_AND,
    OP_OR
} BinOp;

typedef enum {
    UOP_NEG,
    UOP_NOT
} UnOp;

/* A resizable list of expression pointers (call arguments). */
typedef struct {
    struct Expr **items;
    size_t count;
    size_t capacity;
} ExprList;

typedef struct Expr {
    ExprKind kind;
    union {
        struct {
            int value;
        } number;
        struct {
            char name[LEX_MAX_IDENT];
        } var;
        struct {
            UnOp op;
            struct Expr *expr;
        } unary;
        struct {
            BinOp op;
            struct Expr *left;
            struct Expr *right;
        } binary;
        struct {
            char name[LEX_MAX_IDENT];
            ExprList args;
        } call;
        struct {
            char name[LEX_MAX_IDENT];
            struct Expr *index;
        } index;
    } u;
} Expr;

Expr *ast_number(int value);
Expr *ast_var(const char *name);
Expr *ast_unary(UnOp op, Expr *expr);
Expr *ast_binary(BinOp op, Expr *left, Expr *right);
Expr *ast_call(const char *name, ExprList args);   /* takes ownership of args */
Expr *ast_index(const char *name, Expr *index);
void  ast_free(Expr *expr);

void exprlist_init(ExprList *l);
void exprlist_push(ExprList *l, Expr *e);
void exprlist_free(ExprList *l);

/* ---------- Statements ---------- */

typedef enum {
    STMT_EXPR,         /* expr ';'                              */
    STMT_VARDECL,      /* ('int'|'bool') name = expr ';'        */
    STMT_ARRDECL,      /* ('int'|'bool') name [ constexpr ] ';' */
    STMT_ASSIGN,       /* name = expr ';'                       */
    STMT_INDEX_ASSIGN, /* name [ expr ] = expr ';'              */
    STMT_PRINT,        /* print ( expr ) ';'                    */
    STMT_PRINT_STR,    /* print ( "..." ) ';'                   */
    STMT_IF,           /* if (cond) then [else otherwise]       */
    STMT_WHILE,        /* while (cond) body                     */
    STMT_FOR,          /* for (init?; cond?; update?) body      */
    STMT_BREAK,        /* break ';'                             */
    STMT_CONTINUE,     /* continue ';'                          */
    STMT_RETURN,       /* return expr? ';'  (NULL = bare)       */
    STMT_BLOCK         /* { stmt* }                             */
} StmtKind;

typedef struct Stmt Stmt;

/* A resizable list of statement pointers. */
typedef struct {
    Stmt **items;
    size_t count;
    size_t capacity;
} StmtList;

struct Stmt {
    StmtKind kind;
    union {
        struct { Expr *expr; } expr;
        struct { char name[LEX_MAX_IDENT]; Expr *init; } vardecl;
        struct { char name[LEX_MAX_IDENT]; Expr *size; } arrdecl;
        struct { char name[LEX_MAX_IDENT]; Expr *value; } assign;
        struct { char name[LEX_MAX_IDENT]; Expr *index; Expr *value; } index_assign;
        struct { Expr *expr; } print;
        struct { char *text; } print_str;   /* heap copy, owned by the Stmt */
        struct { Expr *cond; Stmt *then_branch; Stmt *else_branch; } if_stmt;
        struct { Expr *cond; Stmt *body; } while_stmt;
        struct { Stmt *init; Expr *cond; Stmt *update; Stmt *body; } for_stmt;
        struct { Expr *value; } ret;        /* NULL for bare `return;` */
        struct { StmtList list; } block;
    } u;
};

Stmt *stmt_expr(Expr *e);
Stmt *stmt_vardecl(const char *name, Expr *init);
Stmt *stmt_arrdecl(const char *name, Expr *size);
Stmt *stmt_assign(const char *name, Expr *value);
Stmt *stmt_index_assign(const char *name, Expr *index, Expr *value);
Stmt *stmt_print(Expr *e);
Stmt *stmt_print_str(const char *text);
Stmt *stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch);
Stmt *stmt_while(Expr *cond, Stmt *body);
Stmt *stmt_for(Stmt *init, Expr *cond, Stmt *update, Stmt *body);
Stmt *stmt_break(void);
Stmt *stmt_continue(void);
Stmt *stmt_return(Expr *value);
Stmt *stmt_block(StmtList list);
void  stmt_free(Stmt *s);

void stmtlist_init(StmtList *l);
void stmtlist_push(StmtList *l, Stmt *s);
void stmtlist_free(StmtList *l);

/* ---------- Top level: function declarations ---------- */

#define FUNC_MAX_PARAMS 16

typedef struct {
    char   name[LEX_MAX_IDENT];
    char   params[FUNC_MAX_PARAMS][LEX_MAX_IDENT];
    size_t param_count;
    Stmt  *body;   /* always a STMT_BLOCK */
} FuncDecl;

typedef struct {
    FuncDecl *items;
    size_t count;
    size_t capacity;
} FuncList;

void funclist_init(FuncList *l);
void funclist_push(FuncList *l, const FuncDecl *f); /* copies *f */
void funclist_free(FuncList *l);

#endif /* AST_H */
