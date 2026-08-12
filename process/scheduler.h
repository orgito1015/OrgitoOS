/*
 * OrgitoOS - Scheduler (Phase 1 + per-CPU / SMP)
 *
 * Preemptive multilevel round-robin scheduler, now per-CPU:
 *   - Every CPU has its own three ready queues (HIGH / NORMAL / LOW,
 *     process/percpu.h cpu_local_t) and runs the head of its own
 *     highest non-empty queue; processes at the same level share that
 *     CPU round-robin via a fixed time quantum. There is no CPU
 *     affinity - a process can end up running on whichever CPU's
 *     schedule() call happens to pick it up next.
 *   - Aging: a READY process that waits longer than AGING_THRESHOLD
 *     ticks on its CPU's queue is temporarily boosted one level so
 *     low-priority work cannot starve. The boost is undone when it runs.
 *   - Timed sleep: SLEEPING processes are woken by whichever CPU's timer
 *     tick next notices their deadline has passed, and enqueued onto
 *     that CPU's own queue - again, no affinity, so this doubles as a
 *     simple form of load spreading.
 *   - Work stealing: when a CPU's own queues are empty and it's about to
 *     fall back to idle, it first checks every other CPU's queues and
 *     steals one process from the busiest before giving up (see
 *     scheduler.c's try_steal_work()).
 *
 * Preemption on CPU 0 (the BSP) is driven by the PIT, exactly as before:
 * timer_handler() calls scheduler_tick() on every tick. Every other CPU
 * (an Application Processor) gets its own preemption source instead - the
 * Local APIC's built-in periodic timer, calibrated once against the PIT
 * and reused on every AP (kernel/apic.c lapic_timer_init(), kernel/smp.c
 * ap_entry()) - since legacy PIT/PIC interrupts are never routed to APs
 * in this kernel (no IOAPIC programming is done). Both paths call the
 * exact same scheduler_tick(), so it's one scheduling policy either way,
 * just triggered by a different interrupt source per CPU.
 *
 * Locking: see process/percpu.h's file header for the single global
 * g_sched_lock this file (and process.c) shares, and schedule()'s own
 * comment below for the handoff protocol around context_switch().
 */

#ifndef ORGITOOS_PROCESS_SCHEDULER_H
#define ORGITOOS_PROCESS_SCHEDULER_H

#include "process.h"

/* Time quantum per process, in timer ticks (100 Hz -> 10 ms/tick) */
#define SCHED_QUANTUM_TICKS  5

/* READY ticks after which a process is boosted one priority level */
#define AGING_THRESHOLD      200

/* Start preemptive scheduling. Call after process_init() and after at
 * least the idle process (PID 0) exists. */
void scheduler_start(void);

/* True once scheduler_start() has run. */
int scheduler_active(void);

/* Called from the timer interrupt on every tick (interrupts off). */
void scheduler_tick(void);

/* Add a process to the ready queue for its priority. */
void scheduler_enqueue(process_t *p);

/* Remove a process from whatever ready queue holds it (no-op if absent). */
void scheduler_dequeue(process_t *p);

/* Pick the next process and context switch to it.
 * Must be called with interrupts disabled. */
void schedule(void);

/* Voluntarily give up the CPU (safe to call with interrupts enabled). */
void scheduler_yield(void);

/* Block the current process (state must be set by caller) and switch. */
void scheduler_block_current(void);

/* Make a blocked/sleeping process runnable again. */
void scheduler_unblock(process_t *p);

/*
 * Finishes a context switch's scheduler-lock handoff: enqueues the
 * process that was switched *out* (cpu_local_t.switching_out), if it's
 * still runnable, then releases g_sched_lock. Called exactly once per
 * switch, as the very first action of whichever process just resumed -
 * schedule()'s own post-context_switch() code for a previously-suspended
 * process, or process.c's kthread_launch()/arch/x86/context.S's
 * fork_child_return for a process running for the very first time. See
 * scheduler.c's file header for why the enqueue can't happen any
 * earlier (a real race with another CPU's work-stealing).
 */
void finish_switch(void);

/* Statistics for `sched` shell command. */
typedef struct {
    uint64_t context_switches;
    uint64_t ticks;
    uint32_t ready_count[PRIORITY_LEVELS];
} sched_stats_t;

void scheduler_get_stats(sched_stats_t *out);

#endif /* ORGITOOS_PROCESS_SCHEDULER_H */
