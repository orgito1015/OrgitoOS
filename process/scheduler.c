/*
 * OrgitoOS - Scheduler Implementation (Phase 1 + per-CPU / SMP)
 *
 * See scheduler.h for the policy description. This file's queues are now
 * per-CPU (queue_head/queue_tail below are macros over
 * this_cpu()->queue_head/queue_tail, process/percpu.h) instead of the
 * single global arrays this code used to have - on a single-CPU boot
 * (smp_get_current_cpu() always 0), that's exactly equivalent to before.
 *
 * Locking protocol (the part worth reading carefully before touching
 * this file): every function below except schedule()/finish_switch()
 * assumes the caller already holds g_sched_lock (with interrupts
 * disabled on this CPU) and never touches the lock itself -
 * scheduler_enqueue()/scheduler_dequeue()/pick_next()/reap_orphans()/
 * apply_aging()/wake_sleepers()/try_steal_work() are all "caller holds
 * the lock" functions, callable both from within schedule() (lock
 * already held) and from external callers like process.c's
 * process_create()/process_exit()/process_kill() or kernel/syscall.c's
 * sys_fork() (which must acquire it themselves first, exactly where they
 * already do `cli` in the single-CPU design - see process.c's file
 * header).
 *
 * schedule() itself is the one place g_sched_lock actually gets
 * released - and only ever via finish_switch(), never by schedule()
 * calling spin_unlock() directly for a real context switch. Why: the
 * process being switched *out* (`prev`) only becomes safe to enqueue
 * (visible to another CPU's work-stealing) once context_switch() has
 * actually finished writing prev->esp - enqueue it any earlier and
 * another CPU could steal it and try to context_switch into it while its
 * esp is still stale, a real and serious race. So the enqueue - and the
 * lock release - are deferred to finish_switch(), called as the very
 * first action of whichever process resumes control right after
 * context_switch() returns: either schedule()'s own code (if that's a
 * previously-suspended process resuming normally), or
 * process.c's kthread_launch() / arch/x86/context.S's fork_child_return
 * (if it's a brand-new process or a freshly-forked child, which never
 * "return into" schedule() at all - they jump straight to their own
 * entry point instead). Exactly one of those three paths runs for any
 * given switch, so finish_switch() runs exactly once per switch.
 *
 * IF (interrupt flag) is deliberately NOT part of this handoff - it's
 * still managed exactly as it always was, purely through
 * context_switch()'s own pushfl/popfl (each process's EFLAGS, including
 * IF, is saved/restored as part of its own callee-saved frame) plus each
 * caller's existing explicit cli/sti bracketing (process.c's
 * irq_disable()/irq_enable() calls, scheduler_yield()'s cli/sti). None
 * of that changes here - only the lock is new.
 */

#include "scheduler.h"
#include "process.h"
#include "percpu.h"
#include "../arch/x86/gdt.h"
#include "../memory/vmm.h"
#include "../drivers/timer.h"
#include "../drivers/console.h"
#include "../include/smp.h"

/* From arch/x86/context.S */
extern void context_switch(uint32_t *old_esp, uint32_t new_esp);

/* Per-CPU ready queues, via process/percpu.h's cpu_local_t - see file
 * header. Scoped to this file only (queue manipulation is scheduler.c's
 * own implementation detail; process.c never touches these directly). */
#define queue_head (this_cpu()->rq_head)
#define queue_tail (this_cpu()->rq_tail)

static int started = 0;

/* ------------------------------------------------------------------ */
/* Ready queues - caller holds g_sched_lock                             */
/* ------------------------------------------------------------------ */

void scheduler_enqueue(process_t *p) {
    if (!p || p->is_idle) return;          /* idle processes never queue */

    p->next       = 0;
    p->wait_ticks = 0;

    uint8_t q = p->priority;
    if (q >= PRIORITY_LEVELS) q = PRIORITY_LEVELS - 1;

    if (queue_tail[q]) {
        queue_tail[q]->next = p;
        queue_tail[q] = p;
    } else {
        queue_head[q] = queue_tail[q] = p;
    }
}

void scheduler_dequeue(process_t *p) {
    if (!p) return;
    /* Since there's no CPU affinity, `p` could be sitting on *any*
     * online CPU's ready queue - not necessarily the calling CPU's
     * (e.g. process_kill() can target a process another CPU's
     * schedule() enqueued or work-stealing moved). Search every CPU's
     * queues; safe to touch them all directly since the caller already
     * holds g_sched_lock, same justification as try_steal_work(). */
    uint32_t n = smp_get_cpu_count();
    for (uint32_t c = 0; c < n; c++) {
        cpu_local_t *cpu = &g_cpu_locals[c];
        for (int q = 0; q < PRIORITY_LEVELS; q++) {
            process_t *prev = 0;
            for (process_t *it = cpu->rq_head[q]; it; prev = it, it = it->next) {
                if (it != p) continue;
                if (prev) prev->next = it->next;
                else      cpu->rq_head[q] = it->next;
                if (cpu->rq_tail[q] == it) cpu->rq_tail[q] = prev;
                it->next = 0;
                return;
            }
        }
    }
}

/* Pop the head of the highest-priority non-empty queue *on this CPU*. */
static process_t *pick_next(void) {
    for (int q = 0; q < PRIORITY_LEVELS; q++) {
        process_t *p = queue_head[q];
        if (p) {
            queue_head[q] = p->next;
            if (!queue_head[q]) queue_tail[q] = 0;
            p->next = 0;
            return p;
        }
    }
    return 0;
}

/*
 * Work stealing: called only from schedule(), only once this CPU's own
 * queues are empty and it's about to fall back to idle. Scans every
 * other online CPU's queues (lowest priority first, so a busy CPU's
 * high-priority work is left alone) and steals the head of the first
 * non-empty one found. Caller holds g_sched_lock, which is what makes
 * directly touching another CPU's queue_head/queue_tail safe here - no
 * other CPU can be concurrently enqueuing/dequeuing anywhere while this
 * runs, by construction of the single global lock (see file header).
 */
static process_t *try_steal_work(void) {
    uint32_t me = smp_get_current_cpu();
    uint32_t n  = smp_get_cpu_count();

    for (uint32_t i = 0; i < n; i++) {
        if (i == me) continue;
        cpu_local_t *other = &g_cpu_locals[i];

        for (int q = PRIORITY_LEVELS - 1; q >= 0; q--) {
            process_t *p = other->rq_head[q];
            if (!p) continue;

            other->rq_head[q] = p->next;
            if (!other->rq_head[q]) other->rq_tail[q] = 0;
            p->next = 0;
            return p;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Housekeeping run inside schedule() - caller holds g_sched_lock       */
/* ------------------------------------------------------------------ */

/*
 * Free the stacks of zombies nobody is waiting for. A zombie whose
 * parent is still alive is kept (its exit code may be collected via
 * process_wait()); once the parent is gone it is released. The process
 * that just called process_exit() is skipped while it is still
 * `current` on this CPU - its kernel stack is in use until we have
 * switched away.
 */
static void reap_orphans(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = process_table_entry(i);
        if (!p || p->state != PROCESS_STATE_ZOMBIE) continue;
        if (p == current_process) continue;

        process_t *parent = process_by_pid(p->ppid);
        if (!parent || parent->state == PROCESS_STATE_UNUSED ||
            parent->state == PROCESS_STATE_ZOMBIE ||
            parent->pid == 0 /* children of idle/kernel auto-reap */) {
            process_release(p);
        }
    }
}

/* Boost long-waiting READY processes on this CPU's queues one level
 * (anti-starvation). */
static void apply_aging(void) {
    for (int q = 1; q < PRIORITY_LEVELS; q++) {
        process_t *it = queue_head[q];
        while (it) {
            process_t *next = it->next;
            it->wait_ticks++;
            if (it->wait_ticks > AGING_THRESHOLD) {
                scheduler_dequeue(it);
                it->priority = (uint8_t)(q - 1);
                scheduler_enqueue(it);   /* resets wait_ticks */
            }
            it = next;
        }
    }
}

/* Wake sleepers (any CPU's) whose deadline has passed, enqueuing them
 * onto *this* CPU's queue - no CPU affinity, so whichever CPU's timer
 * tick notices first just picks them up; see scheduler.h's file header. */
static void wake_sleepers(uint64_t now) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = process_table_entry(i);
        if (!p || p->state != PROCESS_STATE_SLEEPING) continue;
        if (now >= p->sleep_until) {
            p->state = PROCESS_STATE_READY;
            scheduler_enqueue(p);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Core                                                                 */
/* ------------------------------------------------------------------ */

void scheduler_start(void) {
    started = 1;
    console_write("Scheduler: preemptive per-CPU round-robin active (quantum 50 ms)\n");
}

int scheduler_active(void) {
    return started;
}

void finish_switch(void) {
    cpu_local_t *cpu = this_cpu();
    process_t *outgoing = cpu->switching_out;
    cpu->switching_out = 0;

    if (outgoing) {
        scheduler_enqueue(outgoing);
    }
    spin_unlock(&g_sched_lock);
}

/* Caller must already hold g_sched_lock (interrupts disabled on this
 * CPU) - see file header for the full handoff protocol. */
void schedule(void) {
    if (!started) {
        spin_unlock(&g_sched_lock);
        return;
    }

    reap_orphans();

    process_t *prev = current_process;
    process_t *next = pick_next();

    if (!next) {
        /*
         * Nothing on this CPU's own queues. If the current process can
         * keep running, let it; otherwise try to steal work from another
         * CPU before falling back to this CPU's own idle process.
         */
        if (prev->state == PROCESS_STATE_RUNNING) {
            prev->quantum_left = SCHED_QUANTUM_TICKS;
            spin_unlock(&g_sched_lock);
            return;
        }
        next = try_steal_work();
        if (!next) {
            next = this_cpu()->idle;
        }
        if (!next || next == prev) {
            /* Idle blocked and nothing to steal?! Should not happen;
             * just keep running. */
            spin_unlock(&g_sched_lock);
            return;
        }
    }

    /* Requeue the outgoing process if it is still runnable - deferred to
     * finish_switch(), not done here; see file header. */
    int prev_requeue = (prev->state == PROCESS_STATE_RUNNING);
    if (prev_requeue) {
        prev->state = PROCESS_STATE_READY;
        prev->priority = prev->base_priority;   /* Aging boost (if any) is spent */
    }
    this_cpu()->switching_out = prev_requeue ? prev : 0;

    next->state        = PROCESS_STATE_RUNNING;
    next->priority     = next->base_priority;
    next->quantum_left = SCHED_QUANTUM_TICKS;

    current_process = next;
    this_cpu()->context_switches++;

    /*
     * Point this CPU's own TSS at the new process's kernel stack so the
     * next ring 3 -> ring 0 transition (interrupt or int 0x80) on *this*
     * CPU lands on it.
     */
    tss_set_kernel_stack(next->kstack_top ? next->kstack_top : 0);

    /*
     * Switch address space before switching stacks: kernel code/data are
     * mapped identically in every directory (cloned from the kernel's), so
     * continuing to execute here is safe the instant CR3 changes. This must
     * happen before context_switch() so that if `next` is about to IRET
     * back to ring 3, the correct address space is already active.
     */
    vmm_switch_directory(next->page_dir ? next->page_dir : vmm_kernel_directory());

    /* prev != next is guaranteed here: pick_next() never returns a
     * not-yet-enqueued `prev`, and the idle/steal fallback above already
     * bailed out if next == prev. */
    context_switch(&prev->esp, next->esp);
    /* Resumes here only when *this exact* `prev` is chosen to run again
     * by some future schedule() call (on whichever CPU that turns out to
     * be) - brand-new processes and forked children never reach this
     * line at all; see process.c's kthread_launch() and
     * arch/x86/context.S's fork_child_return, which call finish_switch()
     * themselves instead. */
    finish_switch();
}

void scheduler_tick(void) {
    if (!started) return;

    /* Already running with IF=0 here (timer/LAPIC-timer interrupt gate),
     * so just the spinlock is needed - no cli, matching every other
     * caller's discipline of pairing spin_lock with an explicit cli
     * (see arch/x86/spinlock.h's file header on why that pairing
     * matters and why it's unnecessary here specifically). */
    spin_lock(&g_sched_lock);

    uint64_t now = timer_get_ticks();

    current_process->cpu_ticks++;

    wake_sleepers(now);
    apply_aging();

    /* Preempt when the quantum expires or a higher-priority process
     * became runnable on this CPU. */
    int higher_ready = 0;
    for (int q = 0; q < current_process->priority; q++) {
        if (queue_head[q]) { higher_ready = 1; break; }
    }
    /* This CPU's idle process is preempted the moment anything is
     * runnable on it. */
    if (current_process->is_idle) {
        for (int q = 0; q < PRIORITY_LEVELS; q++) {
            if (queue_head[q]) { higher_ready = 1; break; }
        }
    }

    if (current_process->quantum_left > 0) {
        current_process->quantum_left--;
    }

    if (higher_ready || current_process->quantum_left == 0) {
        schedule();   /* releases g_sched_lock - see file header */
        return;
    }

    spin_unlock(&g_sched_lock);
}

void scheduler_yield(void) {
    if (!started) return;

    __asm__ __volatile__("cli");
    spin_lock(&g_sched_lock);
    current_process->quantum_left = 0;
    schedule();   /* releases g_sched_lock */
    __asm__ __volatile__("sti");
}

void scheduler_block_current(void) {
    /* Caller set current->state to BLOCKED/SLEEPING and already holds
     * g_sched_lock (with interrupts disabled) - see process.c. */
    schedule();   /* releases g_sched_lock */
}

void scheduler_unblock(process_t *p) {
    /* Caller holds g_sched_lock. */
    if (!p) return;
    if (p->state == PROCESS_STATE_BLOCKED ||
        p->state == PROCESS_STATE_SLEEPING) {
        p->state = PROCESS_STATE_READY;
        scheduler_enqueue(p);
    }
}

void scheduler_get_stats(sched_stats_t *out) {
    if (!out) return;

    __asm__ __volatile__("cli");
    spin_lock(&g_sched_lock);

    out->context_switches = 0;
    for (int q = 0; q < PRIORITY_LEVELS; q++) out->ready_count[q] = 0;

    uint32_t n = smp_get_cpu_count();
    for (uint32_t i = 0; i < n; i++) {
        cpu_local_t *cpu = &g_cpu_locals[i];
        out->context_switches += cpu->context_switches;
        for (int q = 0; q < PRIORITY_LEVELS; q++) {
            uint32_t cnt = 0;
            for (process_t *it = cpu->rq_head[q]; it; it = it->next) cnt++;
            out->ready_count[q] += cnt;
        }
    }
    out->ticks = timer_get_ticks();

    spin_unlock(&g_sched_lock);
    __asm__ __volatile__("sti");
}
