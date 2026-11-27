/*
 * OrgitoOS - Process Management (Phase 1)
 *
 * Implements the process table, kernel threads, ring 3 processes,
 * exit/kill/wait/sleep. Scheduling policy lives in scheduler.c; the
 * low-level register save/restore lives in arch/x86/context.S.
 *
 * Kernel stack layout for a brand-new kernel thread (top downward):
 *
 *      kstack_top -> [ arg                ]  argument for entry()
 *                    [ process_thread_exit ]  entry()'s return address
 *                    [ entry              ]  context_switch ret target
 *                    [ EFLAGS = 0x202     ]  IF=1 once first scheduled
 *                    [ EBP = 0            ]
 *                    [ EBX = 0            ]
 *                    [ ESI = 0            ]
 *      esp --------> [ EDI = 0            ]
 *
 * context_switch() pops the callee-saved registers and EFLAGS, then
 * `ret` transfers control to entry() which sees a perfectly ordinary
 * cdecl frame: a return address (process_thread_exit) and one argument.
 * If entry() ever returns, it falls into process_thread_exit(), which
 * terminates the process cleanly.
 */

#include "process.h"
#include "../kernel/fileio.h"
#include "scheduler.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../drivers/console.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../arch/x86/gdt.h"
#include "../kernel/string.h"

/* From arch/x86/context.S */
extern void enter_user_mode(uint32_t entry, uint32_t user_stack_top);

/* Process table */
static process_t process_table[PROCESS_MAX];
static uint32_t  next_pid = 1;

/* Currently running process (set up by process_init/scheduler) */
process_t *current_process = 0;

/* Next unused pid. Interrupts must be disabled. Used by fork() too, so
 * pids are never handed out twice (a reused pid could be mistaken for
 * the old process by a parent waiting on it). */
uint32_t process_next_pid(void) {
    return next_pid++;
}

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

/* Find a free table slot. Interrupts must be disabled. */
static process_t *alloc_pcb(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_STATE_UNUSED) {
            process_t *p = &process_table[i];
            /* Clear the whole PCB */
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
     * as PID 0: the idle process. It never sits in a ready queue; the
     * scheduler falls back to it when nothing else is runnable.
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
    copy_name(idle, "idle");

    current_process = idle;

    /* Boot-time status only - kept off the narrow split-pane screen, still
     * visible via serial (see kernel/kernel.c's boot banner comment). */
    serial_write("Process: table initialized (64 slots), PID 0 = idle\n");
}

/* ------------------------------------------------------------------ */
/* Kernel threads                                                       */
/* ------------------------------------------------------------------ */

/* Landing pad if a thread's entry function returns. */
static void process_thread_exit(void) {
    process_exit(0);
}

/*
 * Shared PCB-building core for process_create()/process_create_user().
 *
 * When defer_activate is set, the new PCB is returned with interrupts
 * still disabled and NOT yet enqueued/runnable: the caller must finish
 * populating any additional fields (process_create_user's page_dir,
 * ustack_top, user_entry, is_user) and then call process_activate().
 * This closes a real race where, without deferral, process_create()
 * would enqueue the PCB and re-enable interrupts before
 * process_create_user() had set those fields - a timer tick landing in
 * that window could schedule the process and enter_user_mode() with a
 * still-zeroed user_entry/ustack_top, IRETing to a garbage EIP.
 */
static process_t *process_create_internal(const char *name, process_entry_t entry,
                                          void *arg, uint8_t priority,
                                          int defer_activate) {
    if (!entry || priority >= PRIORITY_LEVELS) {
        return 0;
    }

    uint8_t *stack = (uint8_t *)kmalloc(PROCESS_KSTACK_SIZE);
    if (!stack) {
        return 0;
    }

    irq_disable();

    process_t *p = alloc_pcb();
    if (!p) {
        irq_enable();
        kfree(stack);
        return 0;
    }

    copy_name(p, name);
    p->ppid          = current_process ? current_process->pid : 0;
    /* Inherit the parent's console pane (a program started from the
     * right-hand shell prints and reads there). Shell threads override
     * this themselves in shell_task(). */
    p->pane          = current_process ? current_process->pane : 0;
    p->priority      = priority;
    p->base_priority = priority;
    p->quantum_left  = SCHED_QUANTUM_TICKS;
    p->kstack        = stack;
    p->kstack_top    = (uint32_t)stack + PROCESS_KSTACK_SIZE;
    p->is_user       = 0;
    /* Kernel threads share the kernel's address space directly; only user
     * processes (process_create_user, below) get their own directory. */
    p->page_dir      = vmm_kernel_directory();

    /* Build the initial kernel stack frame (see file header comment). */
    uint32_t *sp = (uint32_t *)p->kstack_top;
    *(--sp) = (uint32_t)arg;                  /* entry's argument        */
    *(--sp) = (uint32_t)process_thread_exit;  /* entry's return address  */
    *(--sp) = (uint32_t)entry;                /* context_switch target   */
    *(--sp) = 0x202;                          /* EFLAGS (IF = 1)         */
    *(--sp) = 0;                              /* EBP                     */
    *(--sp) = 0;                              /* EBX                     */
    *(--sp) = 0;                              /* ESI                     */
    *(--sp) = 0;                              /* EDI                     */
    p->esp = (uint32_t)sp;

    if (defer_activate) {
        /* Caller must finish setup and call process_activate(p); IRQs
         * stay disabled until then so the scheduler cannot touch p. */
        return p;
    }

    p->state = PROCESS_STATE_READY;
    scheduler_enqueue(p);

    irq_enable();
    return p;
}

/* Make a deferred-activate PCB (see process_create_internal) runnable.
 * Must be called exactly once, after finishing any additional setup,
 * with interrupts still disabled from the process_create_internal() call
 * that returned p. */
static void process_activate(process_t *p) {
    p->state = PROCESS_STATE_READY;
    scheduler_enqueue(p);
    irq_enable();
}

process_t *process_create(const char *name, process_entry_t entry,
                          void *arg, uint8_t priority) {
    return process_create_internal(name, entry, arg, priority, 0);
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

static process_t *create_user_common(const char *name, uint32_t entry,
                                     uint8_t priority, int suspended) {
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

    /* Deferred activation: p is returned with IRQs disabled and NOT yet
     * enqueued, so the scheduler cannot run it until process_activate()
     * below - see process_create_internal()'s header comment for why
     * that matters here specifically (is_user/page_dir/ustack_top/
     * user_entry must all be set before this PCB is ever scheduled). */
    process_t *p = process_create_internal(name, user_process_trampoline, 0,
                                           priority, 1);
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
    p->user_entry = entry;

    if (suspended) {
        /* Not runnable until process_start(): exec() maps the ELF image
         * first. Inherit the creator's strace settings, like strace -f. */
        if (current_process) {
            p->trace_mask = current_process->trace_mask;
            p->trace_out  = current_process->trace_out;
        }
        p->state = PROCESS_STATE_BLOCKED;
        irq_enable();
        return p;
    }

    process_activate(p);
    return p;
}

process_t *process_create_user(const char *name, void (*entry)(void),
                               uint8_t priority) {
    return create_user_common(name, (uint32_t)entry, priority, 0);
}

process_t *process_create_user_suspended(const char *name, uint32_t entry,
                                         uint8_t priority) {
    return create_user_common(name, entry, priority, 1);
}

void process_start(process_t *p) {
    irq_disable();
    if (p && p->state == PROCESS_STATE_BLOCKED) {
        p->state = PROCESS_STATE_READY;
        scheduler_enqueue(p);
    }
    irq_enable();
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
    /* Pages of a loaded ELF image (kernel/exec.c). The tracking array
     * itself is kfree'd in process_release(). */
    for (uint32_t i = 0; i < p->exec_page_count; i++) {
        if (p->exec_pages && p->exec_pages[i].phys) {
            pmm_free_page((void *)p->exec_pages[i].phys);
            p->exec_pages[i].phys = 0;
        }
    }
}

void process_exit(int code) {
    irq_disable();

    process_t *self = current_process;

    self->exit_code = code;
    self->state     = PROCESS_STATE_ZOMBIE;

    /* Release any files the process left open (like a Unix exit()). */
    fileio_close_all(self);

    /* User stack can be freed now; the kernel stack is still in use
     * until we have switched away, so the reaper frees it later. */
    free_ustack_pages(self);

    /* Wake a parent blocked in process_wait(). */
    process_t *parent = process_by_pid(self->ppid);
    if (parent && parent->parent_waiting &&
        parent->state == PROCESS_STATE_BLOCKED) {
        scheduler_unblock(parent);
    }

    schedule();

    /* Unreachable */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

int process_kill(uint32_t pid) {
    if (pid == 0) {
        return -1;  /* Cannot kill the idle process */
    }

    irq_disable();

    process_t *p = process_by_pid(pid);
    if (!p || p->state == PROCESS_STATE_UNUSED ||
        p->state == PROCESS_STATE_ZOMBIE) {
        irq_enable();
        return -1;
    }

    if (p == current_process) {
        irq_enable();
        process_exit(-1);           /* Never returns */
    }

    /* Remove from wherever it is queued and mark it a zombie. */
    scheduler_dequeue(p);
    p->exit_code = -1;
    p->state     = PROCESS_STATE_ZOMBIE;

    free_ustack_pages(p);

    /* Wake a waiting parent, as in process_exit(). */
    process_t *parent = process_by_pid(p->ppid);
    if (parent && parent->parent_waiting &&
        parent->state == PROCESS_STATE_BLOCKED) {
        scheduler_unblock(parent);
    }

    irq_enable();
    return 0;
}

int process_wait(int *status) {
    return process_waitpid(-1, status);
}

int process_waitpid(int pid_wanted, int *status) {
    for (;;) {
        irq_disable();

        int have_children = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            process_t *p = &process_table[i];
            if (p->state == PROCESS_STATE_UNUSED) continue;
            if (p->ppid != current_process->pid || p == current_process) continue;
            if (pid_wanted != -1 && (int)p->pid != pid_wanted) continue;

            have_children = 1;
            if (p->state == PROCESS_STATE_ZOMBIE) {
                int pid = (int)p->pid;
                if (status) *status = p->exit_code;
                process_release(p);      /* Reap: frees stacks, slot   */
                irq_enable();
                return pid;
            }
        }

        if (!have_children) {
            irq_enable();
            return -1;
        }

        /* Block until a child exits. */
        current_process->parent_waiting = 1;
        current_process->state = PROCESS_STATE_BLOCKED;
        scheduler_block_current();
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

    /* 100 Hz timer -> 10 ms per tick; round up. */
    uint64_t ticks = (ms + 9) / 10;
    current_process->sleep_until = timer_get_ticks() + ticks;
    current_process->state = PROCESS_STATE_SLEEPING;
    scheduler_block_current();

    irq_enable();
}

/* ------------------------------------------------------------------ */
/* Queries / bookkeeping                                                */
/* ------------------------------------------------------------------ */

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
    if (p->exec_pages) {
        kfree(p->exec_pages);
        p->exec_pages = 0;
    }
    p->exec_page_count = 0;
    p->state = PROCESS_STATE_UNUSED;
    p->pid   = 0;
}
