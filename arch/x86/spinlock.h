/*
 * OrgitoOS - Minimal SMP Spinlock
 *
 * A plain test-and-set spinlock using GCC's __sync builtins (lower to a
 * locked `xchg`/equivalent on x86, available since the 8086 - no need for
 * -march=i486+ cmpxchg). Not fair (no ticket ordering) and not
 * reentrant - a CPU that already holds a lock must never try to acquire
 * it again (see process/scheduler.c's g_sched_lock usage for the
 * discipline this codebase follows to avoid that).
 *
 * Every acquisition in this codebase is paired with `cli` on the same
 * CPU before spin_lock() (see callers) - a spinlock alone only excludes
 * *other* CPUs; it does nothing to stop this same CPU's own interrupt
 * handler from re-entering the critical section on top of the code that
 * currently holds the lock, which would self-deadlock (spin forever
 * waiting for a lock this exact CPU already holds, with nothing else
 * able to run and release it).
 */

#ifndef ORGITOOS_ARCH_X86_SPINLOCK_H
#define ORGITOOS_ARCH_X86_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_init(spinlock_t *l) {
    l->locked = 0;
}

static inline void spin_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked) {
            __asm__ __volatile__("pause");
        }
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __sync_lock_release(&l->locked);
}

#endif /* ORGITOOS_ARCH_X86_SPINLOCK_H */
