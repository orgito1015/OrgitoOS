/*
 * OrgitoOS - System Call Implementation (Phase 1)
 *
 * Dispatches int 0x80 requests. The most involved call is fork():
 *
 *   1. Allocate a child PCB and kernel stack, and a private address space
 *      (its own page directory, cloned from the kernel's, plus fresh
 *      physical pages for its user stack).
 *   2. Copy the parent's user stack content into the child's pages. Because
 *      every user process maps its stack at the same fixed virtual address
 *      (PROCESS_USTACK_VIRT_BASE), the child's stack ends up at *exactly*
 *      the same virtual addresses the parent's was at - so pointers and
 *      saved EBP frames inside the copied bytes are already correct as-is;
 *      unlike a flat shared address space, no relocation is needed.
 *   3. Build the child's kernel stack: a copy of the parent's syscall
 *      register frame with EAX = 0 (fork returns 0 in the child), sitting
 *      above a context_switch frame whose return target is
 *      fork_child_return. When the scheduler first picks the child (after
 *      switching CR3 to the child's own directory), context_switch
 *      "returns" into that stub, which unwinds the register frame and
 *      IRETs straight back to the instruction after the parent's
 *      `int $0x80` - now running in the child's own address space.
 */

#include "syscall.h"
#include "kernel.h"
#include "../process/process.h"
#include "../process/scheduler.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../drivers/console.h"
#include "../arch/x86/idt.h"
#include "../arch/x86/gdt.h"

/* Scratch virtual address used to briefly map a freshly-allocated child
 * stack page into the parent's own (currently active) address space so it
 * can be memcpy'd into - sits right after the fixed user-stack window, in
 * the same private, per-process PDE range, so mapping/unmapping it here
 * never touches any other process's or the kernel's shared page tables. */
#define FORK_SCRATCH_VIRT (PROCESS_USTACK_VIRT_BASE + PROCESS_USTACK_SIZE)

/* Assembly stubs */
extern void int80_handler(void);      /* arch/x86/syscall.S  */
extern void fork_child_return(void);  /* arch/x86/context.S  */

void syscall_init(void) {
    /*
     * DPL 3 interrupt gate: user code may execute int 0x80, and the
     * gate type clears IF on entry so the frame is captured atomically.
     */
    idt_set_gate(0x80, (uint32_t)int80_handler,
                 KERNEL_CODE_SEGMENT, IDT_FLAGS_USER);
    console_write("Syscalls: int 0x80 gate installed (8 syscalls)\n");
}

/* ------------------------------------------------------------------ */
/* sys_write                                                            */
/* ------------------------------------------------------------------ */

static int sys_write(const char *str) {
    if (!str) return -1;
    int n = 0;
    /* Bounded to keep a bad pointer from wedging the console forever. */
    while (str[n] && n < 4096) {
        console_put_char(str[n]);
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* sys_fork                                                             */
/* ------------------------------------------------------------------ */

static int sys_fork(regs_t *parent_regs) {
    process_t *parent = process_current();

    if (!parent->is_user || !parent->page_dir) {
        return -1;   /* fork() is only defined for user processes */
    }

    uint8_t *child_kstack = (uint8_t *)kmalloc(PROCESS_KSTACK_SIZE);
    if (!child_kstack) return -1;

    struct page_directory *child_dir = vmm_create_directory();
    if (!child_dir) {
        kfree(child_kstack);
        return -1;
    }
    vmm_clone_kernel_mappings(child_dir);

    /* Allocate the child's private stack pages and copy the parent's stack
     * content into them. Both stacks live at the identical virtual address
     * (PROCESS_USTACK_VIRT_BASE), so unlike a flat shared address space, no
     * pointer relocation is needed - only the bytes need to be copied. The
     * source read comes straight from the parent's own mapping (its
     * directory is the one currently active in CR3); the destination write
     * goes through a temporary scratch mapping in the parent's directory,
     * since the freshly allocated physical page isn't reachable at any
     * address until it's mapped somewhere. */
    uint32_t child_ustack_phys[PROCESS_USTACK_PAGES];
    for (int i = 0; i < PROCESS_USTACK_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            for (int j = 0; j < i; j++) pmm_free_page((void *)child_ustack_phys[j]);
            vmm_destroy_directory(child_dir);
            kfree(child_kstack);
            return -1;
        }
        child_ustack_phys[i] = (uint32_t)phys;

        vmm_map_page(parent->page_dir, (void *)FORK_SCRATCH_VIRT, (uint32_t)phys,
                     PTE_PRESENT | PTE_WRITABLE);
        uint8_t *src = (uint8_t *)(PROCESS_USTACK_VIRT_BASE + i * PAGE_SIZE);
        uint8_t *dst = (uint8_t *)FORK_SCRATCH_VIRT;
        for (uint32_t b = 0; b < PAGE_SIZE; b++) dst[b] = src[b];
        vmm_unmap_page(parent->page_dir, (void *)FORK_SCRATCH_VIRT);

        vmm_map_page(child_dir, (void *)(PROCESS_USTACK_VIRT_BASE + i * PAGE_SIZE),
                     (uint32_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    __asm__ __volatile__("cli");

    /* PCB */
    process_t *child = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *slot = process_table_entry(i);
        if (slot && slot->state == PROCESS_STATE_UNUSED) {
            child = slot;
            break;
        }
    }
    if (!child) {
        __asm__ __volatile__("sti");
        for (int i = 0; i < PROCESS_USTACK_PAGES; i++) pmm_free_page((void *)child_ustack_phys[i]);
        vmm_destroy_directory(child_dir);
        kfree(child_kstack);
        return -1;
    }

    /* Start from a copy of the parent's PCB, then fix up identity. */
    *child = *parent;
    child->next           = 0;
    child->parent_waiting = 0;
    child->cpu_ticks      = 0;
    child->wait_ticks     = 0;

    /* New pid: reuse process_by_pid-safe allocation via a scan. */
    {
        uint32_t pid = 1;
        for (;;) {
            int taken = 0;
            for (int i = 0; i < PROCESS_MAX; i++) {
                process_t *s = process_table_entry(i);
                if (s && s != child && s->state != PROCESS_STATE_UNUSED &&
                    s->pid == pid) {
                    taken = 1;
                    break;
                }
            }
            if (!taken) break;
            pid++;
        }
        child->pid = pid;
    }
    child->ppid = parent->pid;

    /* Stacks and address space */
    child->kstack     = child_kstack;
    child->kstack_top = (uint32_t)child_kstack + PROCESS_KSTACK_SIZE;
    child->page_dir   = child_dir;
    for (int i = 0; i < PROCESS_USTACK_PAGES; i++) child->ustack_phys[i] = child_ustack_phys[i];
    child->ustack_top = parent->ustack_top;  /* identical fixed virtual window */

    /* 3. Build the child's kernel stack. */
    uint32_t sp = child->kstack_top;

    /* 3a. Copied register frame (top of kernel stack). EAX=0 is the only
     * change needed - useresp/ebp are virtual addresses in the shared fixed
     * stack window, valid unmodified in the child's own address space. */
    sp -= sizeof(regs_t);
    regs_t *child_regs = (regs_t *)sp;
    *child_regs = *parent_regs;
    child_regs->eax = 0;                            /* fork() -> 0      */

    /* 3b. context_switch frame beneath it. */
    uint32_t *csp = (uint32_t *)sp;
    *(--csp) = (uint32_t)fork_child_return;  /* ret target              */
    *(--csp) = 0x002;                        /* EFLAGS: IF=0 until IRET */
    *(--csp) = 0;                            /* EBP                     */
    *(--csp) = 0;                            /* EBX                     */
    *(--csp) = 0;                            /* ESI                     */
    *(--csp) = 0;                            /* EDI                     */
    child->esp = (uint32_t)csp;

    child->state = PROCESS_STATE_READY;
    scheduler_enqueue(child);

    __asm__ __volatile__("sti");
    return (int)child->pid;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                           */
/* ------------------------------------------------------------------ */

void syscall_dispatch(regs_t *r) {
    switch (r->eax) {
        case SYS_EXIT:
            process_exit((int)r->ebx);          /* Never returns */
            break;

        case SYS_WRITE:
            r->eax = (uint32_t)sys_write((const char *)r->ebx);
            break;

        case SYS_GETPID:
            r->eax = process_getpid();
            break;

        case SYS_FORK:
            r->eax = (uint32_t)sys_fork(r);
            break;

        case SYS_YIELD:
            r->eax = 0;
            scheduler_yield();
            break;

        case SYS_SLEEP:
            r->eax = 0;
            process_sleep(r->ebx);
            break;

        case SYS_GETPPID:
            r->eax = process_getppid();
            break;

        case SYS_WAIT: {
            int status = 0;
            int pid = process_wait(&status);
            if (r->ebx) {
                *(int *)r->ebx = status;
            }
            r->eax = (uint32_t)pid;
            break;
        }

        default:
            r->eax = (uint32_t)-1;
            break;
    }
}
