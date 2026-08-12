/*
 * OrgitoOS - Process Management (Phase 1 + per-CPU scheduling)
 *
 * Implements the process table, kernel threads, ring 3 processes,
 * exit/kill/wait/sleep. Scheduling policy lives in scheduler.c; the
 * low-level register save/restore lives in arch/x86/context.S.
 *
 * Locking: every mutation of the shared process table (alloc_pcb() and
 * every state transition below) happens under g_sched_lock (see
 * process/percpu.h) with interrupts disabled on the calling CPU - the
 * same critical sections this file always cli/sti'd around in the
 * single-CPU design, just now also excluding other CPUs. Paths that call
 * schedule() (directly or via scheduler_block_current()) do NOT
 * explicitly release g_sched_lock afterward - see scheduler.c's
 * schedule()/finish_switch() for why release is deferred to whichever
 * process resumes next, not to this call site.
 *
 * Kernel stack layout for a brand-new kernel thread (top downward):
 *
 *      kstack_top -> [ EFLAGS = 0x202     ]  IF=1 once first scheduled
 *                    [ EBP = 0            ]
 *                    [ EBX = 0            ]
 *                    [ ESI = 0            ]
 *      esp --------> [ EDI = 0            ]
 *
 * context_switch() pops the callee-saved registers and EFLAGS, then
 * `ret` transfers control to kthread_launch() - an ordinary C function
 * (not a hand-crafted fake return frame the way `entry` used to be
 * jumped to directly): it finishes the scheduler handoff
 * (finish_switch()) and then makes a normal call to whatever entry/arg
 * process_create() stashed in the PCB (p->pending_entry/pending_arg).
 * This indirection - through a PCB field and a real function call,
 * rather than baking `entry`'s address and argument directly into the
 * initial stack frame - exists so kthread_launch() has a place to run
 * finish_switch() before any process-supplied code executes; see
 * scheduler.c's file header for why that has to happen on the resuming
 * side, not in schedule() itself.
 */

#include "process.h"
#include "percpu.h"
#include "scheduler.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../drivers/console.h"
#include "../drivers/timer.h"
#include "../arch/x86/gdt.h"
#include "../kernel/string.h"

/* From arch/x86/context.S */
extern void enter_user_mode(uint32_t entry, uint32_t user_stack_top);

/* Process table - shared across every CPU, protected by g_sched_lock. */
static process_t process_table[PROCESS_MAX];
static uint32_t  next_pid = 1;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static inline void irq_disable(void) { __asm__ __volatile__("cli"); }
static inline void irq_enable(void)  { __asm__ __volatile__("sti"); }

static void copy_name(process_t *p, const char *name) {
    size_t i = 0;
    if (name) {
        while (name[i] && i < PROCESS_NAME_LEN - 1) {
            p->name[i] = name[i];
            i++;
        }
    }
    p->name[i] = '\0';
}

/* Find a free table slot. Caller must hold g_sched_lock with interrupts
 * disabled. */
static process_t *alloc_pcb(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_STATE_UNUSED) {
            process_t *p = &process_table[i];
            uint8_t *b = (uint8_t *)p;
            for (size_t j = 0; j < sizeof(*p); j++) b[j] = 0;
            p->pid = next_pid++;
            return p;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                       */
/* ------------------------------------------------------------------ */

void process_init(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_table[i].state = PROCESS_STATE_UNUSED;
    }

    /*
     * Adopt the current boot context (kmain, running on the boot stack)
     * as PID 0: CPU 0's (the BSP's) idle process. It never sits in a
     * ready queue; the scheduler falls back to it when nothing else -
     * including stolen work from another CPU - is runnable on this CPU.
     */
    process_t *idle = &process_table[0];
    uint8_t *b = (uint8_t *)idle;
    for (size_t j = 0; j < sizeof(*idle); j++) b[j] = 0;

    idle->pid           = 0;
    idle->ppid          = 0;
    idle->state         = PROCESS_STATE_RUNNING;
    idle->priority      = PRIORITY_LOW;
    idle->base_priority = PRIORITY_LOW;
    idle->quantum_left  = SCHED_QUANTUM_TICKS;
    idle->page_dir      = vmm_kernel_directory();
    idle->is_idle       = 1;
    copy_name(idle, "idle");

    g_cpu_locals[0].proc = idle;
    g_cpu_locals[0].idle = idle;

    console_write("Process: table initialized (64 slots), PID 0 = idle (CPU 0)\n");
}

void process_init_ap(uint32_t cpu_id) {
    irq_disable();
    spin_lock(&g_sched_lock);

    process_t *idle = alloc_pcb();
    if (!idle) {
        /* Out of PCB slots - should not happen at boot with only a
         * handful of CPUs and PROCESS_MAX=64, but fail safe: this AP
         * simply has no idle process to fall back to, which schedule()
         * already guards against (NULL idle -> just keeps running
         * whatever's "current", same as if nothing were runnable). */
        spin_unlock(&g_sched_lock);
        irq_enable();
        return;
    }

    idle->ppid          = 0;
    idle->state          = PROCESS_STATE_RUNNING;
    idle->priority       = PRIORITY_LOW;
    idle->base_priority  = PRIORITY_LOW;
    idle->quantum_left   = SCHED_QUANTUM_TICKS;
    idle->page_dir       = vmm_kernel_directory();
    idle->is_idle        = 1;
    copy_name(idle, "idle");

    g_cpu_locals[cpu_id].proc = idle;
    g_cpu_locals[cpu_id].idle = idle;

    spin_unlock(&g_sched_lock);
    irq_enable();
}

/* ------------------------------------------------------------------ */
/* Kernel threads                                                       */
/* ------------------------------------------------------------------ */

/* Landing pad if a thread's entry function returns. */
static void process_thread_exit(void) {
    process_exit(0);
}

/* First C code a brand-new kernel thread ever runs (see file header
 * comment). Never called directly - only ever reached via
 * context_switch()'s `ret` landing on the address process_create()
 * stashed at the top of a fresh kernel stack. */
static void kthread_launch(void) {
    finish_switch();

    process_t *self = current_process;
    process_entry_t entry = self->pending_entry;
    void *arg = self->pending_arg;

    entry(arg);
    process_thread_exit();
}

process_t *process_create(const char *name, process_entry_t entry,
                          void *arg, uint8_t priority) {
    if (!entry || priority >= PRIORITY_LEVELS) {
        return 0;
    }

    uint8_t *stack = (uint8_t *)kmalloc(PROCESS_KSTACK_SIZE);
    if (!stack) {
        return 0;
    }

    irq_disable();
    spin_lock(&g_sched_lock);

    process_t *p = alloc_pcb();
    if (!p) {
        spin_unlock(&g_sched_lock);
        irq_enable();
        kfree(stack);
        return 0;
    }

    copy_name(p, name);
    p->ppid          = current_process ? current_process->pid : 0;
    p->priority      = priority;
    p->base_priority = priority;
    p->quantum_left  = SCHED_QUANTUM_TICKS;
    p->kstack        = stack;
    p->kstack_top    = (uint32_t)stack + PROCESS_KSTACK_SIZE;
    p->is_user       = 0;
    /* Kernel threads share the kernel's address space directly; only user
     * processes (process_create_user, below) get their own directory. */
    p->page_dir      = vmm_kernel_directory();
    p->pending_entry = entry;
    p->pending_arg   = arg;

    /* Build the initial kernel stack frame (see file header comment). */
    uint32_t *sp = (uint32_t *)p->kstack_top;
    *(--sp) = (uint32_t)kthread_launch;       /* context_switch target   */
    *(--sp) = 0x202;                          /* EFLAGS (IF = 1)         */
    *(--sp) = 0;                              /* EBP                     */
    *(--sp) = 0;                              /* EBX                     */
    *(--sp) = 0;                              /* ESI                     */
    *(--sp) = 0;                              /* EDI                     */
    p->esp = (uint32_t)sp;

    p->state = PROCESS_STATE_READY;
    scheduler_enqueue(p);

    spin_unlock(&g_sched_lock);
    irq_enable();
    return p;
}

/* ------------------------------------------------------------------ */
/* User-mode processes                                                  */
/* ------------------------------------------------------------------ */

/*
 * Kernel-side trampoline for ring 3 processes: runs as a fresh kernel
 * thread, then drops to user mode. The user entry/stack were stashed in
 * the PCB by process_create_user().
 */
static void user_process_trampoline(void *arg) {
    (void)arg;
    process_t *self = current_process;
    enter_user_mode(self->user_entry, self->ustack_top);
    /* Not reached: user code exits via sys_exit(). */
}

process_t *process_create_user(const char *name, void (*entry)(void),
                               uint8_t priority) {
    /*
     * Give this process its own address space: a directory cloned from the
     * kernel's (so code/heap stay reachable, unchanged from today) plus a
     * private, non-shared physical backing for its user stack at the fixed
     * PROCESS_USTACK_VIRT_BASE window. Because that window sits above
     * vmm_init()'s identity-map cap, the kernel directory has never touched
     * its page-directory slot, so vmm_map_page() below allocates a table
     * exclusive to `dir` - other processes' directories never see it.
     */
    struct page_directory *dir = vmm_create_directory();
    if (!dir) {
        return 0;
    }
    vmm_clone_kernel_mappings(dir);

    uint32_t ustack_phys[PROCESS_USTACK_PAGES];
    for (int i = 0; i < PROCESS_USTACK_PAGES; i++) {
        void *phys = pmm_alloc_page();
        if (!phys) {
            for (int j = 0; j < i; j++) {
                pmm_free_page((void *)ustack_phys[j]);
            }
            vmm_destroy_directory(dir);
            return 0;
        }
        ustack_phys[i] = (uint32_t)phys;
        vmm_map_page(dir, (void *)(PROCESS_USTACK_VIRT_BASE + i * PAGE_SIZE),
                     (uint32_t)phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    }

    process_t *p = process_create(name, user_process_trampoline, 0, priority);
    if (!p) {
        for (int i = 0; i < PROCESS_USTACK_PAGES; i++) {
            pmm_free_page((void *)ustack_phys[i]);
        }
        vmm_destroy_directory(dir);
        return 0;
    }

    p->is_user  = 1;
    p->page_dir = dir;
    for (int i = 0; i < PROCESS_USTACK_PAGES; i++) {
        p->ustack_phys[i] = ustack_phys[i];
    }
    /* Keep ESP 16-byte aligned and leave a small red zone at the top. */
    p->ustack_top = ((PROCESS_USTACK_VIRT_BASE + PROCESS_USTACK_SIZE) & ~0xFu) - 16;
    p->user_entry = (uint32_t)entry;
    return p;
}

/* ------------------------------------------------------------------ */
/* Exit / kill / wait / sleep                                           */
/* ------------------------------------------------------------------ */

/* Reclaim a user process's private stack pages. Safe to call while the
 * process is still `current`, since by this point execution is entirely on
 * the kernel stack - the user stack (and the address space it lives in)
 * is no longer in use. The private page table backing it is reclaimed
 * later, when process_release() destroys the whole directory. */
static void free_ustack_pages(process_t *p) {
    if (!p->is_user) return;
    for (int i = 0; i < PROCESS_USTACK_PAGES; i++) {
        if (p->ustack_phys[i]) {
            pmm_free_page((void *)p->ustack_phys[i]);
            p->ustack_phys[i] = 0;
        }
    }
}

/* Mirror of free_ustack_pages() for the physical pages backing an
 * exec()'d ELF image (kernel/exec.c), if this process ever called
 * exec(). Safe no-op for processes that never did. */
static void free_exec_pages(process_t *p) {
    if (!p->exec_pages) return;
    for (uint32_t i = 0; i < p->exec_page_count; i++) {
        if (p->exec_pages[i].phys) {
            pmm_free_page((void *)p->exec_pages[i].phys);
        }
    }
    kfree(p->exec_pages);
    p->exec_pages = 0;
    p->exec_page_count = 0;
}

void process_exit(int code) {
    irq_disable();
    spin_lock(&g_sched_lock);

    process_t *self = current_process;

    self->exit_code = code;
    self->state     = PROCESS_STATE_ZOMBIE;

    /* User stack (and any exec()'d image) can be freed now; the kernel
     * stack is still in use until we have switched away, so the reaper
     * frees it later. */
    free_ustack_pages(self);
    free_exec_pages(self);

    /* Wake a parent blocked in process_wait(). */
    process_t *parent = process_by_pid(self->ppid);
    if (parent && parent->parent_waiting &&
        parent->state == PROCESS_STATE_BLOCKED) {
        scheduler_unblock(parent);
    }

    schedule();   /* Never returns for this call frame; g_sched_lock is
                   * released via finish_switch() on whichever process
                   * resumes next - see scheduler.c. */

    /* Unreachable */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

int process_kill(uint32_t pid) {
    irq_disable();
    spin_lock(&g_sched_lock);

    process_t *p = process_by_pid(pid);
    if (!p || p->is_idle || p->state == PROCESS_STATE_UNUSED ||
        p->state == PROCESS_STATE_ZOMBIE) {
        spin_unlock(&g_sched_lock);
        irq_enable();
        return -1;
    }

    if (p == current_process) {
        spin_unlock(&g_sched_lock);
        irq_enable();
        process_exit(-1);           /* Never returns */
    }

    /* Remove from wherever it is queued and mark it a zombie. */
    scheduler_dequeue(p);
    p->exit_code = -1;
    p->state     = PROCESS_STATE_ZOMBIE;

    free_ustack_pages(p);
    free_exec_pages(p);

    /* Wake a waiting parent, as in process_exit(). */
    process_t *parent = process_by_pid(p->ppid);
    if (parent && parent->parent_waiting &&
        parent->state == PROCESS_STATE_BLOCKED) {
        scheduler_unblock(parent);
    }

    spin_unlock(&g_sched_lock);
    irq_enable();
    return 0;
}

int process_wait(int *status) {
    for (;;) {
        irq_disable();
        spin_lock(&g_sched_lock);

        int have_children = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            process_t *p = &process_table[i];
            if (p->state == PROCESS_STATE_UNUSED) continue;
            if (p->ppid != current_process->pid || p == current_process) continue;

            have_children = 1;
            if (p->state == PROCESS_STATE_ZOMBIE) {
                int pid = (int)p->pid;
                if (status) *status = p->exit_code;
                process_release(p);      /* Reap: frees stacks, slot   */
                spin_unlock(&g_sched_lock);
                irq_enable();
                return pid;
            }
        }

        if (!have_children) {
            spin_unlock(&g_sched_lock);
            irq_enable();
            return -1;
        }

        /* Block until a child exits. */
        current_process->parent_waiting = 1;
        current_process->state = PROCESS_STATE_BLOCKED;
        scheduler_block_current();   /* g_sched_lock released via
                                       * finish_switch() by the time this
                                       * returns - see scheduler.c. */
        current_process->parent_waiting = 0;
        irq_enable();
    }
}

void process_sleep(uint32_t ms) {
    if (ms == 0) {
        scheduler_yield();
        return;
    }

    irq_disable();
    spin_lock(&g_sched_lock);

    /* 100 Hz timer -> 10 ms per tick; round up. */
    uint64_t ticks = (ms + 9) / 10;
    current_process->sleep_until = timer_get_ticks() + ticks;
    current_process->state = PROCESS_STATE_SLEEPING;
    scheduler_block_current();   /* releases g_sched_lock - see above */

    irq_enable();
}

/* ------------------------------------------------------------------ */
/* Queries / bookkeeping                                                */
/* ------------------------------------------------------------------ */

/*
 * Lock-free by design: these are simple reads used both by already-locked
 * callers above (safe: reading under a lock you already hold is always
 * fine) and by unlocked callers like the `ps`/`sched` shell commands
 * (display-only - the same benign eventual-consistency the single-CPU
 * version of this code already tolerated, e.g. a process shown as
 * "running" that exited a moment ago).
 */
process_t *process_current(void)  { return current_process; }
uint32_t   process_getpid(void)   { return current_process ? current_process->pid : 0; }
uint32_t   process_getppid(void)  { return current_process ? current_process->ppid : 0; }

process_t *process_by_pid(uint32_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED &&
            process_table[i].pid == pid) {
            return &process_table[i];
        }
    }
    return 0;
}

process_t *process_table_entry(int index) {
    if (index < 0 || index >= PROCESS_MAX) return 0;
    return &process_table[index];
}

const char *process_state_name(process_state_t s) {
    switch (s) {
        case PROCESS_STATE_UNUSED:   return "unused";
        case PROCESS_STATE_READY:    return "ready";
        case PROCESS_STATE_RUNNING:  return "running";
        case PROCESS_STATE_BLOCKED:  return "blocked";
        case PROCESS_STATE_SLEEPING: return "sleeping";
        case PROCESS_STATE_ZOMBIE:   return "zombie";
        default:                     return "?";
    }
}

int process_count(void) {
    int n = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_STATE_UNUSED) n++;
    }
    return n;
}

void process_release(process_t *p) {
    if (!p || p == current_process) return;

    if (p->kstack) {
        kfree(p->kstack);
        p->kstack = 0;
    }
    /* free_ustack_pages() already reclaimed the physical stack pages in
     * process_exit()/process_kill(); the directory (and the now-empty
     * private page table that mapped them) is only safe to tear down here,
     * once the process is fully done running. */
    if (p->page_dir && p->page_dir != vmm_kernel_directory()) {
        vmm_destroy_directory(p->page_dir);
    }
    p->page_dir = 0;
    p->state = PROCESS_STATE_UNUSED;
    p->pid   = 0;
}
