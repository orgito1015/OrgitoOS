/*
 * OrgitoOS - Per-CPU Scheduler State (storage)
 *
 * See percpu.h for the design. All-zero initial state is correct:
 * current_process/idle/switching_out start NULL, queues start empty,
 * exactly like the single global state this replaces used to.
 */

#include "percpu.h"

cpu_local_t g_cpu_locals[MAX_CPUS];
spinlock_t  g_sched_lock = SPINLOCK_INIT;
