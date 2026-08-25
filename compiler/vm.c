/* OrgitoCompiler VM - ported from OrionCC. See oc_platform.h for the
 * freestanding-kernel substitutions (malloc/free, exit->OC_FAIL, INT_MIN,
 * and printf("%d\n"/"%s\n") -> oc_eprintf(), which is just a general
 * console_write-backed formatter despite the "e" in its name - the VM's
 * print()/print_str runtime output reuses it rather than duplicating a
 * second formatter). */
#include "oc_platform.h"
#include "vm.h"

#define STACK_MAX  1024

/* Locals region shared by all live call frames (each frame is a window
 * [frame base, frame base + nlocals) into it). Replaces the old flat
 * vars[VAR_MAX=256]: because codegen reuses slots when block scopes end,
 * a single frame never needs more than its high-water mark, and 65536
 * slots comfortably holds one maximal frame or many small recursive ones. */
#define LOCALS_MAX 65536

/* Maximum call depth. Runaway recursion hits this and fails with a clean
 * "VM error: call stack overflow" instead of overflowing the native C stack
 * -- the runtime counterpart of the parser's PARSER_MAX_DEPTH guard. */
#define CALL_MAX   1024

typedef struct {
    size_t ret_ip;     /* where BC_RET resumes in the caller       */
    int    saved_fb;   /* caller's frame base                      */
    int    saved_top;  /* caller's locals top (== this frame's fb) */
} Frame;

static int truthy(int v) { return v != 0; }

/* Registered while a run is in flight so oc_vm_cleanup_after_error() can
 * free them if OC_FAIL longjmps out mid-run (see vm.h). NULL otherwise. */
static int   *g_vm_locals = NULL;
static Frame *g_vm_frames = NULL;

void oc_vm_cleanup_after_error(void) {
    oc_free(g_vm_locals);
    oc_free(g_vm_frames);
    g_vm_locals = NULL;
    g_vm_frames = NULL;
}

int vm_execute(const Bytecode *bc) {
    int stack[STACK_MAX];

    int *locals = (int *)oc_malloc(LOCALS_MAX * sizeof(int));
    Frame *frames = (Frame *)oc_malloc(CALL_MAX * sizeof(Frame));
    g_vm_locals = locals;
    g_vm_frames = frames;
    if (!locals || !frames) {
        oc_vm_cleanup_after_error();
        OC_FAIL("VM error: out of memory\n");
    }
    memset(locals, 0, LOCALS_MAX * sizeof(int));

    int sp = 0;
    size_t ip = 0;
    int fb = 0;          /* current frame base into locals[]              */
    int locals_top = 0;  /* first free locals slot                        */
    int frame_depth = 0; /* number of live call frames                    */
    int last = 0;        /* last value popped by BC_POP / stored / printed */
    int have_last = 0;

    while (ip < bc->count) {
        int op = bc->data[ip++];

        switch (op) {
            case BC_PUSH_CONST: {
                if (ip >= bc->count) {
                    OC_FAIL("Bytecode error: missing constant after BC_PUSH_CONST\n");
                }
                int value = bc->data[ip++];
                if (sp >= STACK_MAX) { OC_FAIL("VM error: stack overflow\n"); }
                stack[sp++] = value;
                break;
            }
            case BC_ADD: {
                if (sp < 2) { OC_FAIL("VM error: stack underflow on ADD\n"); }
                int b = stack[--sp]; int a = stack[--sp];
                stack[sp++] = (int)((unsigned)a + (unsigned)b);
                break;
            }
            case BC_SUB: {
                if (sp < 2) { OC_FAIL("VM error: stack underflow on SUB\n"); }
                int b = stack[--sp]; int a = stack[--sp];
                stack[sp++] = (int)((unsigned)a - (unsigned)b);
                break;
            }
            case BC_MUL: {
                if (sp < 2) { OC_FAIL("VM error: stack underflow on MUL\n"); }
                int b = stack[--sp]; int a = stack[--sp];
                stack[sp++] = (int)((unsigned)a * (unsigned)b);
                break;
            }
            case BC_DIV: {
                if (sp < 2) { OC_FAIL("VM error: stack underflow on DIV\n"); }
                int b = stack[--sp]; int a = stack[--sp];
                if (b == 0) { OC_FAIL("VM error: division by zero\n"); }
                /* Guard INT_MIN / -1 which is signed overflow UB. */
                if (a == OC_INT_MIN && b == -1) {
                    OC_FAIL("VM error: integer overflow in division (INT_MIN / -1)\n");
                }
                stack[sp++] = a / b; /* C truncates toward zero */
                break;
            }
            case BC_MOD: {
                if (sp < 2) { OC_FAIL("VM error: stack underflow on MOD\n"); }
                int b = stack[--sp]; int a = stack[--sp];
                if (b == 0) { OC_FAIL("VM error: modulo by zero\n"); }
                if (a == OC_INT_MIN && b == -1) {
                    /* Result is mathematically 0, but a/b would overflow;
                     * define it as 0 directly. */
                    stack[sp++] = 0;
                } else {
                    stack[sp++] = a % b; /* sign follows dividend (toward zero) */
                }
                break;
            }
            case BC_NEG: {
                if (sp < 1) { OC_FAIL("VM error: stack underflow on NEG\n"); }
                int a = stack[--sp];
                /* Negate via unsigned arithmetic to avoid signed overflow UB
                 * when a == INT_MIN. The result -INT_MIN wraps to INT_MIN,
                 * which matches two's-complement expectation. */
                stack[sp++] = (int)(0u - (unsigned)a);
                break;
            }
            case BC_LT: case BC_GT: case BC_LE:
            case BC_GE: case BC_EQ: case BC_NE: {
                if (sp < 2) { OC_FAIL("VM error: stack underflow on comparison\n"); }
                int b = stack[--sp]; int a = stack[--sp];
                int r = 0;
                switch (op) {
                    case BC_LT: r = a <  b; break;
                    case BC_GT: r = a >  b; break;
                    case BC_LE: r = a <= b; break;
                    case BC_GE: r = a >= b; break;
                    case BC_EQ: r = a == b; break;
                    case BC_NE: r = a != b; break;
                }
                stack[sp++] = r ? 1 : 0;
                break;
            }
            case BC_NOT: {
                if (sp < 1) { OC_FAIL("VM error: stack underflow on NOT\n"); }
                int a = stack[--sp];
                stack[sp++] = truthy(a) ? 0 : 1;
                break;
            }
            case BC_LOAD: {
                if (ip >= bc->count) { OC_FAIL("Bytecode error: missing slot after BC_LOAD\n"); }
                int slot = bc->data[ip++];
                if (slot < 0 || fb + slot >= locals_top) { OC_FAIL("VM error: variable slot out of range\n"); }
                if (sp >= STACK_MAX) { OC_FAIL("VM error: stack overflow\n"); }
                stack[sp++] = locals[fb + slot];
                break;
            }
            case BC_STORE: {
                if (ip >= bc->count) { OC_FAIL("Bytecode error: missing slot after BC_STORE\n"); }
                int slot = bc->data[ip++];
                if (slot < 0 || fb + slot >= locals_top) { OC_FAIL("VM error: variable slot out of range\n"); }
                if (sp < 1) { OC_FAIL("VM error: stack underflow on STORE\n"); }
                int v = stack[--sp];
                locals[fb + slot] = v;
                last = v; have_last = 1;
                break;
            }
            case BC_LOAD_IDX: {
                if (ip + 1 >= bc->count) { OC_FAIL("Bytecode error: missing operands after BC_LOAD_IDX\n"); }
                int base = bc->data[ip++];
                int size = bc->data[ip++];
                if (sp < 1) { OC_FAIL("VM error: stack underflow on LOAD_IDX\n"); }
                int idx = stack[--sp];
                if (idx < 0 || idx >= size) {
                    OC_FAIL("VM error: array index out of bounds (index %d, size %d)\n", idx, size);
                }
                if (base < 0 || size < 1 || fb + base + size > locals_top) {
                    OC_FAIL("VM error: variable slot out of range\n");
                }
                stack[sp++] = locals[fb + base + idx];
                break;
            }
            case BC_STORE_IDX: {
                if (ip + 1 >= bc->count) { OC_FAIL("Bytecode error: missing operands after BC_STORE_IDX\n"); }
                int base = bc->data[ip++];
                int size = bc->data[ip++];
                if (sp < 2) { OC_FAIL("VM error: stack underflow on STORE_IDX\n"); }
                int idx = stack[--sp];
                int v = stack[--sp];
                if (idx < 0 || idx >= size) {
                    OC_FAIL("VM error: array index out of bounds (index %d, size %d)\n", idx, size);
                }
                if (base < 0 || size < 1 || fb + base + size > locals_top) {
                    OC_FAIL("VM error: variable slot out of range\n");
                }
                locals[fb + base + idx] = v;
                last = v; have_last = 1;
                break;
            }
            case BC_ZERO: {
                if (ip + 1 >= bc->count) { OC_FAIL("Bytecode error: missing operands after BC_ZERO\n"); }
                int base = bc->data[ip++];
                int size = bc->data[ip++];
                if (base < 0 || size < 1 || fb + base + size > locals_top) {
                    OC_FAIL("VM error: variable slot out of range\n");
                }
                memset(&locals[fb + base], 0, (size_t)size * sizeof(int));
                break;
            }
            case BC_CALL: {
                if (ip >= bc->count) { OC_FAIL("Bytecode error: missing function index after BC_CALL\n"); }
                int fi = bc->data[ip++];
                if (fi < 0 || (size_t)fi >= bc->func_count) {
                    OC_FAIL("Bytecode error: invalid function index %d\n", fi);
                }
                const BcFunc *f = &bc->funcs[fi];
                if (f->entry < 0 || (size_t)f->entry >= bc->count) {
                    OC_FAIL("Bytecode error: invalid function entry offset\n");
                }
                if (frame_depth >= CALL_MAX) {
                    OC_FAIL("VM error: call stack overflow\n");
                }
                if (f->nlocals < 0 || locals_top > LOCALS_MAX - f->nlocals) {
                    OC_FAIL("VM error: locals stack overflow\n");
                }
                if (sp < f->arity) {
                    OC_FAIL("VM error: stack underflow on CALL\n");
                }
                frames[frame_depth].ret_ip = ip;
                frames[frame_depth].saved_fb = fb;
                frames[frame_depth].saved_top = locals_top;
                frame_depth++;

                int new_fb = locals_top;
                /* Args were pushed left to right, so pop them into the
                 * parameter slots back to front. */
                for (int i = f->arity - 1; i >= 0; i--) {
                    locals[new_fb + i] = stack[--sp];
                }
                if (f->nlocals > f->arity) {
                    memset(&locals[new_fb + f->arity], 0,
                           (size_t)(f->nlocals - f->arity) * sizeof(int));
                }
                fb = new_fb;
                locals_top = new_fb + f->nlocals;
                ip = (size_t)f->entry;
                break;
            }
            case BC_RET: {
                if (frame_depth == 0) {
                    OC_FAIL("VM error: return outside of a function call\n");
                }
                frame_depth--;
                ip = frames[frame_depth].ret_ip;
                fb = frames[frame_depth].saved_fb;
                locals_top = frames[frame_depth].saved_top;
                /* The return value stays on the value stack for the caller. */
                break;
            }
            case BC_PRINT: {
                if (sp < 1) { OC_FAIL("VM error: stack underflow on PRINT\n"); }
                int v = stack[--sp];
                oc_eprintf("%d\n", v);
                last = v; have_last = 1;
                break;
            }
            case BC_PRINT_STR: {
                if (ip >= bc->count) { OC_FAIL("Bytecode error: missing string index after BC_PRINT_STR\n"); }
                int idx = bc->data[ip++];
                if (idx < 0 || (size_t)idx >= bc->str_count) {
                    OC_FAIL("Bytecode error: invalid string constant index %d\n", idx);
                }
                oc_eprintf("%s\n", bc->strs[idx]);
                break;
            }
            case BC_POP: {
                if (sp < 1) { OC_FAIL("VM error: stack underflow on POP\n"); }
                last = stack[--sp]; have_last = 1;
                break;
            }
            case BC_JMP: {
                if (ip >= bc->count) { OC_FAIL("Bytecode error: missing target after BC_JMP\n"); }
                int target = bc->data[ip++];
                if (target < 0 || (size_t)target > bc->count) { OC_FAIL("VM error: jump target out of range\n"); }
                ip = (size_t)target;
                break;
            }
            case BC_JZ: {
                if (ip >= bc->count) { OC_FAIL("Bytecode error: missing target after BC_JZ\n"); }
                int target = bc->data[ip++];
                if (sp < 1) { OC_FAIL("VM error: stack underflow on JZ\n"); }
                int cond = stack[--sp];
                if (target < 0 || (size_t)target > bc->count) { OC_FAIL("VM error: jump target out of range\n"); }
                if (!truthy(cond)) ip = (size_t)target;
                break;
            }
            case BC_HALT: {
                /* In expression-only mode, the result is the value left on
                 * the stack. In program mode, HALT follows the prologue's
                 * CALL to main, so the stack top is main's return value. */
                int result;
                if (sp >= 1) result = stack[sp - 1];
                else if (have_last) result = last;
                else result = 0;
                oc_vm_cleanup_after_error(); /* frees + clears g_vm_locals/frames */
                return result;
            }
            default:
                OC_FAIL("VM error: unknown opcode %d\n", op);
        }
    }

    oc_vm_cleanup_after_error();
    OC_FAIL("VM error: no BC_HALT encountered\n");
    return 0; /* unreachable: OC_FAIL never returns */
}
