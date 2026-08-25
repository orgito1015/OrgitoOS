#ifndef CODEGEN_H
#define CODEGEN_H

#include <stddef.h>
#include "ast.h"

typedef enum {
    BC_PUSH_CONST = 0,
    BC_ADD,
    BC_SUB,
    BC_MUL,
    BC_DIV,
    BC_MOD,
    BC_NEG,
    /* comparison: push 0/1 */
    BC_LT,
    BC_GT,
    BC_LE,
    BC_GE,
    BC_EQ,
    BC_NE,
    /* logical */
    BC_NOT,       /* unary: 0 -> 1, nonzero -> 0 */
    /* variables (frame-relative; see calling convention below) */
    BC_LOAD,      /* operand: frame-relative slot; push locals[fb+slot]      */
    BC_STORE,     /* operand: frame-relative slot; pop value, store          */
    /* io */
    BC_PRINT,     /* pop value, print "<value>\n"                            */
    /* control flow */
    BC_JMP,       /* operand: absolute target ip                             */
    BC_JZ,        /* operand: absolute target ip; pop, jump if 0             */
    BC_POP,       /* discard top of stack                                    */
    BC_HALT,
    /* --- opcodes below were appended after BC_HALT so that all pre-existing
     *     opcode values are unchanged --- */
    /* functions */
    BC_CALL,      /* operand: function index into Bytecode.funcs             */
    BC_RET,       /* pop the current frame; return value stays on the stack  */
    /* arrays (frame-relative base + compile-time size; bounds-checked) */
    BC_LOAD_IDX,  /* operands: base slot, size; pop index, push element      */
    BC_STORE_IDX, /* operands: base slot, size; pop index, pop value, store  */
    BC_ZERO,      /* operands: base slot, size; zero locals[fb+base..+size)  */
    /* strings */
    BC_PRINT_STR  /* operand: index into Bytecode.strs; print "<str>\n"      */
} OpCode;

/*
 * Calling convention
 * ==================
 *
 * Bytecode layout for a program:
 *
 *     ip 0:  BC_CALL <main_index>
 *     ip 2:  BC_HALT
 *     ip 3+: function bodies, in declaration order
 *
 * Function signatures (name, arity) are collected in a first pass before any
 * body is generated, so functions may call each other regardless of
 * declaration order and recursion needs no forward-reference patching: call
 * sites emit a stable *function index*, and the VM resolves the entry offset
 * through Bytecode.funcs at runtime (entries are filled in during the second
 * pass, before the VM ever runs).
 *
 * Call sequence:
 *   1. The caller evaluates arguments LEFT TO RIGHT, pushing each onto the
 *      value stack (so the last argument is on top).
 *   2. BC_CALL <func_index>:
 *        - pushes a frame onto a call-frame stack SEPARATE from the value
 *          stack, saving {return ip, caller's frame base, caller's locals
 *          top} -- return addresses never live on the value stack, so user
 *          code can't clobber them;
 *        - pops the callee's `arity` arguments off the value stack into the
 *          callee's first `arity` local slots (parameters ARE locals 0..n-1);
 *        - zeroes the callee's remaining local slots (arrays and any slot
 *          read defensively start at 0);
 *        - sets the frame base to the first free locals slot and jumps to
 *          the function's entry offset.
 *   3. BC_LOAD / BC_STORE / BC_LOAD_IDX / BC_STORE_IDX operands are
 *      FRAME-RELATIVE offsets, resolved against the current frame base.
 *   4. BC_RET pops the frame (restoring the caller's ip, frame base, and
 *      locals top) and leaves exactly one value -- the return value -- on
 *      the value stack above the caller's temporaries. A function that
 *      falls off its end returns 0: codegen appends `PUSH_CONST 0; RET` to
 *      every body, and a bare `return;` pushes 0 explicitly.
 *
 * Local-variable slots are assigned per function by a scoped symbol table
 * (see codegen.c). Slots are REUSED when a block scope ends, so sibling
 * scopes share slots and deep block nesting does not grow the frame; each
 * function's frame size is the high-water mark of live slots, recorded in
 * Bytecode.funcs[i].nlocals and used by the VM to size the frame. This is
 * why the old flat VAR_MAX=256 limit did not need raising: the per-frame
 * limit is FRAME_SLOTS_MAX below, and frames stack in the VM's locals
 * region (see vm.c LOCALS_MAX / CALL_MAX for the runtime guards).
 */

/* Per-function frame slot budget; must not exceed the VM's LOCALS_MAX. */
#define FRAME_SLOTS_MAX 65536

typedef struct {
    int entry;   /* absolute bytecode offset of the function body */
    int arity;   /* number of parameters                          */
    int nlocals; /* frame size in slots (params + locals + arrays)*/
} BcFunc;

typedef struct {
    int   *data;
    size_t count;
    size_t capacity;
    /* String constant pool. Bytecode.data stays a flat int stream (existing
     * BC_* layout untouched); BC_PRINT_STR's operand indexes into this
     * parallel structure instead of trying to fit text into ints. */
    char **strs;
    size_t str_count;
    size_t str_capacity;
    /* Function table; BC_CALL's operand indexes into this. */
    BcFunc *funcs;
    size_t  func_count;
    size_t  func_capacity;
} Bytecode;

void bc_init(Bytecode *bc);
void bc_emit(Bytecode *bc, int value);
int  bc_add_string(Bytecode *bc, const char *text); /* returns pool index */
int  bc_add_func(Bytecode *bc, int arity);          /* returns func index */
void bc_free(Bytecode *bc);

/* Legacy single-expression codegen (expression-only mode). */
void codegen_expr(Bytecode *bc, Expr *expr);

/* Program codegen: emits the CALL-main prologue, BC_HALT, and every function
 * body. Errors (undeclared variables, arity mismatches, missing main, ...)
 * are reported and exit the process. */
void codegen_program(Bytecode *bc, const FuncList *program);

void codegen_finish(Bytecode *bc);

#endif /* CODEGEN_H */
