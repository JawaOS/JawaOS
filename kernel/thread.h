#ifndef JAWA_THREAD_H
#define JAWA_THREAD_H

#include <stdint.h>
#include <stdbool.h>

#include "spinlock.h"

struct vm_as;

/* Task states. */
#define TASK_READY    1   /* on a runqueue, waiting for the CPU */
#define TASK_RUNNING  2   /* currently executing on this CPU    */
#define TASK_SLEEPING 3   /* parked on a wait queue             */
#define TASK_ZOMBIE   4   /* exited, awaiting reap              */

/* Priority runqueues: 0 = highest, SCHED_PRIO_MAX = lowest. A task at
 * priority p gets a quantum of SCHED_BASE_QUANTUM >> p ticks. */
#define SCHED_NQUEUE       4
#define SCHED_PRIO_MAX     (SCHED_NQUEUE - 1)
#define SCHED_BASE_QUANTUM 4u

/* Wait queue: tasks parked for a condition or a timeout. There is no
 * separate lock — every wait-queue operation runs with interrupts
 * disabled (irq_save inside the primitives, already-off inside ISRs),
 * and callers hold their condition spinlock across the check-and-block
 * so wakeups cannot be lost. */
typedef struct wq {
    struct task *head;      /* FIFO of TASK_SLEEPING tasks */
} wq_t;

typedef struct task {
    uint64_t sp;
    struct task *next;      /* runqueue / wait-queue / zombie link */
    struct task *tnext;     /* timer (deadline) list link */
    struct vm_as *as;       /* process address space; NULL = kernel-only */

    uint8_t state;
    uint8_t prio;
    uint8_t quantum;        /* ticks left in the current slice */
    uint64_t cpu_ticks;     /* total ticks consumed */

    uint64_t wake_at;       /* absolute ticks_ms deadline; 0 = none */
    struct wq *wait_wq;     /* queue this task is parked on */
    bool timed_out;         /* set by the deadline path */

    const char *name;
    void *stk_base;         /* heap-allocated stack, for reap */
    bool stk_heap;
} task_t;

extern task_t *current;

void thread_init(void);
task_t *kthread_create(void (*fn)(uint64_t), uint64_t arg);
void thread_set_priority(task_t *t, unsigned prio);
void yield(void);
void schedule(void);
void sched_tick(void);
void thread_exit(void) __attribute__((noreturn));
void thread_reap(void);

/* Wait queues. The caller holds its condition spinlock across the
 * check-and-block; the primitive releases it atomically with parking. */
void wq_init(wq_t *wq);
void wq_block(wq_t *wq, spinlock_t *lk);
bool wq_block_timed(wq_t *wq, uint64_t ms, spinlock_t *lk);
void wq_wake_one(wq_t *wq);
void wq_wake_all(wq_t *wq);

/* Sleep without holding any condition lock (deadline-based). */
void thread_sleep_ms(uint64_t ms);

#endif
