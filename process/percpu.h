/*
 * OrgitoOS - Per-CPU Scheduler State
 *
 * Before this file existed, `current_process` and the ready queues
 * (process/scheduler.c) were single global state, correct only because
 * exactly one CPU (the BSP) ever ran process code - kernel/smp.c's APs
 * parked in `cli;hlt` forever. Making APs actually run processes means
 * each CPU needs its own notion of "what's running here" and "what's
 * ready to run here."
 *
 * `current_process` is redefined below as a macro over
 * `g_cpu_locals[smp_get_current_cpu()].proc` - deliberately a
 * macro, not a rename, so every existing bare `current_process` reference
 * throughout process.c/scheduler.c (there are many) automatically becomes
 * per-CPU with zero risk of missing a call site. It degrades to exactly
 * today's single-global behavior whenever smp_get_current_cpu() reports
 * single-CPU (always returns 0 - see kernel/smp.c), which is also why
 * `current_process` is safe to use as a macro even in expressions like
 * `current_process = next;` (it's a genuine assignable lvalue, just one
 * that lives in an array).
 *
 * Locking: every one of the fields below (this CPU's queues, and the
 * global process table declared in process.h) is protected by ONE global
 * spinlock, g_sched_lock - not one lock per CPU. A per-CPU-lock design
 * was seriously considered and rejected: work-stealing inherently needs
 * to reach into *another* CPU's queue, and mixing per-CPU queue locks
 * with a separate global process-table lock opens the door to lock-
 * ordering deadlocks (CPU A holds its own queue lock while waiting for
 * the table lock; CPU B holds the table lock while waiting for A's queue
 * lock) that are extremely easy to get wrong and, without the ability to
 * boot-test this kernel, effectively unverifiable. A single global lock
 * sacrifices fine-grained parallelism *inside the scheduler* but keeps
 * the design provably deadlock-free by construction, while still letting
 * every CPU run genuinely independent process code in parallel between
 * scheduling events - which is where SMP's real payoff is.
 *
 * See process/scheduler.c's schedule()/finish_switch() for the locking
 * protocol itself (who acquires g_sched_lock, who releases it, and why
 * release is deferred to whichever process resumes next rather than
 * happening in schedule() unconditionally).
 */

#ifndef ORGITOOS_PROCESS_PERCPU_H
#define ORGITOOS_PROCESS_PERCPU_H

#include "process.h"
#include "../include/smp.h"
#include "../arch/x86/spinlock.h"

typedef struct cpu_local {
    /* Named `proc`, not `current_process` - `current_process` is a macro
     * (below) that expands to `this_cpu()->proc`; if this field were
     * also named `current_process`, any `x.current_process` elsewhere
     * would itself macro-expand into the nonsensical
     * `x.(this_cpu()->current_process)`. */
    process_t *proc;

    /* Ready queues. Named `rq_head`/`rq_tail`, not `queue_head`/
     * `queue_tail` - process/scheduler.c used to have those as bare
     * file-local statics and, for readability, aliases them back via its
     * own local `#define queue_head this_cpu()->rq_head` (scoped to that
     * one file, unlike `current_process` above which needs to be a
     * cross-file macro). Naming the actual fields differently from that
     * alias avoids the exact same self-referential expansion problem
     * `proc`'s doc comment above describes for current_process. */
    process_t *rq_head[PRIORITY_LEVELS];
    process_t *rq_tail[PRIORITY_LEVELS];
    uint64_t   context_switches;

    /* This CPU's own dedicated idle process (process_t.is_idle == 1),
     * created by process_init() for the BSP (cpu 0) and by
     * process_init_ap() for every Application Processor as it comes
     * online (kernel/smp.c ap_entry()). Never enqueued; schedule() falls
     * back to it when nothing else - including stolen work - is
     * runnable on this CPU. */
    process_t *idle;

    /* Set by schedule() immediately before context_switch(), naming the
     * process being switched *out* (or NULL if it isn't going back on a
     * ready queue). Consumed by finish_switch(), which runs as the very
     * first action of whichever process resumes next - see
     * process/scheduler.c for why the enqueue is deferred this far. */
    process_t *switching_out;
} cpu_local_t;

extern cpu_local_t g_cpu_locals[MAX_CPUS];
extern spinlock_t  g_sched_lock;

static inline cpu_local_t *this_cpu(void) {
    return &g_cpu_locals[smp_get_current_cpu()];
}

#define current_process (this_cpu()->proc)

#endif /* ORGITOOS_PROCESS_PERCPU_H */
