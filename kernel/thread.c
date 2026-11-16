#include <stdint.h>
#include <stddef.h>

#include "console.h"
#include "kheap.h"
#include "spinlock.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "vm.h"

task_t *current;

#define STACK_WORDS 512
#define STATIC_STACKS 3

static bool sched_active;
static uint64_t task_stacks[STATIC_STACKS][STACK_WORDS] __attribute__((aligned(16)));
static int static_stacks_used;

/* Priority runqueues: FIFO per level, pick scans highest first. */
static task_t *rq_head[SCHED_NQUEUE];
static task_t *rq_tail[SCHED_NQUEUE];

/* Tasks parked with a deadline (wake_at != 0), newest first. */
static task_t *timer_list;

/* Exited tasks awaiting thread_reap(). */
static task_t *zombies;

#if defined(__x86_64__)

__attribute__((naked))
static void switch_to(uint64_t *save_sp __attribute__((unused)),
                      uint64_t new_sp __attribute__((unused)))
{
    __asm__ volatile (
        "pushq %rbp\n"
        "pushq %rbx\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "movq %rsp, (%rdi)\n"
        "movq %rsi, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %rbx\n"
        "popq %rbp\n"
        "retq\n");
}

__attribute__((naked))
static void thread_trampoline(void)
{
    __asm__ volatile ("popq %rax\n\tpopq %rdi\n\tcallq *%rax\n\tcallq thread_exit\n");
}

static uint64_t make_frame(uint64_t *top, uint64_t fn, uint64_t arg)
{
    uint64_t *p = top;
    *--p = arg;
    *--p = fn;
    *--p = (uint64_t)thread_trampoline;
    *--p = 0;
    *--p = 0;
    *--p = 0;
    *--p = 0;
    *--p = 0;
    *--p = 0;
    return (uint64_t)p;
}

#elif defined(__aarch64__)

__attribute__((naked))
static void switch_to(uint64_t *save_sp __attribute__((unused)),
                      uint64_t new_sp __attribute__((unused)))
{
    __asm__ volatile (
        "stp x19, x20, [sp, #-16]!\n"
        "stp x21, x22, [sp, #-16]!\n"
        "stp x23, x24, [sp, #-16]!\n"
        "stp x25, x26, [sp, #-16]!\n"
        "stp x27, x28, [sp, #-16]!\n"
        "stp x29, x30, [sp, #-16]!\n"
        "mov x2, sp\n"
        "str x2, [x0]\n"
        "mov sp, x1\n"
        "ldp x29, x30, [sp], #16\n"
        "ldp x27, x28, [sp], #16\n"
        "ldp x25, x26, [sp], #16\n"
        "ldp x23, x24, [sp], #16\n"
        "ldp x21, x22, [sp], #16\n"
        "ldp x19, x20, [sp], #16\n"
        "ret\n");
}

__attribute__((naked))
static void thread_trampoline(void)
{
    __asm__ volatile ("ldr x16, [sp], #8\n\tldr x0, [sp], #8\n\tblr x16\n\tb thread_exit\n");
}

static uint64_t make_frame(uint64_t *top, uint64_t fn, uint64_t arg)
{
    uint64_t *p = top;
    *--p = arg;
    *--p = fn;
    for (int i = 0; i < 10; i++)
        *--p = 0;
    *--p = (uint64_t)thread_trampoline;
    *--p = 0;
    return (uint64_t)p;
}

#elif defined(__riscv)

__attribute__((naked))
static void switch_to(uint64_t *save_sp __attribute__((unused)),
                      uint64_t new_sp __attribute__((unused)))
{
    __asm__ volatile (
        "addi sp, sp, -104\n"
        "sd ra, 0(sp)\n"
        "sd s0, 8(sp)\n"
        "sd s1, 16(sp)\n"
        "sd s2, 24(sp)\n"
        "sd s3, 32(sp)\n"
        "sd s4, 40(sp)\n"
        "sd s5, 48(sp)\n"
        "sd s6, 56(sp)\n"
        "sd s7, 64(sp)\n"
        "sd s8, 72(sp)\n"
        "sd s9, 80(sp)\n"
        "sd s10, 88(sp)\n"
        "sd s11, 96(sp)\n"
        "sd sp, 0(a0)\n"
        "mv sp, a1\n"
        "ld ra, 0(sp)\n"
        "ld s0, 8(sp)\n"
        "ld s1, 16(sp)\n"
        "ld s2, 24(sp)\n"
        "ld s3, 32(sp)\n"
        "ld s4, 40(sp)\n"
        "ld s5, 48(sp)\n"
        "ld s6, 56(sp)\n"
        "ld s7, 64(sp)\n"
        "ld s8, 72(sp)\n"
        "ld s9, 80(sp)\n"
        "ld s10, 88(sp)\n"
        "ld s11, 96(sp)\n"
        "addi sp, sp, 104\n"
        "ret\n");
}

__attribute__((naked))
static void thread_trampoline(void)
{
    __asm__ volatile ("ld t0, 0(sp)\n\tld a0, 8(sp)\n\taddi sp, sp, 16\n\tjalr t0\n\tcall thread_exit\n");
}

static uint64_t make_frame(uint64_t *top, uint64_t fn, uint64_t arg)
{
    uint64_t *p = top;
    *--p = arg;
    *--p = fn;
    for (int i = 0; i < 12; i++)
        *--p = 0;
    *--p = (uint64_t)thread_trampoline;
    return (uint64_t)p;
}

#else
#error "unsupported architecture"
#endif

void thread_init(void)
{
    static task_t main_task;
    memset(&main_task, 0, sizeof main_task);
    main_task.sp = 0;
    main_task.as = NULL;
    main_task.state = TASK_RUNNING;
    main_task.prio = SCHED_PRIO_MAX;
    main_task.quantum = SCHED_BASE_QUANTUM;
    main_task.name = "kmain";
    current = &main_task;
    sched_active = true;
}

static uint8_t quantum_for(unsigned prio)
{
    uint8_t q = SCHED_BASE_QUANTUM >> prio;
    return q ? q : 1;
}

/* All runqueue/wait-queue mutations run with interrupts disabled —
 * either by the caller's own irq_save or by these primitives. */
static void sched_enqueue(task_t *t)
{
    t->next = NULL;
    if (rq_tail[t->prio])
        rq_tail[t->prio]->next = t;
    else
        rq_head[t->prio] = t;
    rq_tail[t->prio] = t;
}

static task_t *sched_pick(void)
{
    for (unsigned p = 0; p < SCHED_NQUEUE; p++) {
        task_t *t = rq_head[p];
        if (t != NULL) {
            rq_head[p] = t->next;
            if (rq_tail[p] == t)
                rq_tail[p] = NULL;
            t->next = NULL;
            return t;
        }
    }
    return NULL;
}

static void timer_unlink(task_t *t)
{
    task_t **pp = &timer_list;
    while (*pp && *pp != t)
        pp = &(*pp)->tnext;
    if (*pp)
        *pp = t->tnext;
    t->tnext = NULL;
}

static void wq_unlink(wq_t *wq, task_t *t)
{
    task_t **pp = &wq->head;
    while (*pp && *pp != t)
        pp = &(*pp)->next;
    if (*pp)
        *pp = t->next;
}

/* Park current on wq (optionally with a deadline). Interrupts must be
 * off on entry. */
static void block_on(wq_t *wq, uint64_t timeout_ms)
{
    current->state = TASK_SLEEPING;
    current->next = NULL;
    if (wq->head == NULL) {
        wq->head = current;
    } else {
        task_t *t = wq->head;
        while (t->next != NULL)
            t = t->next;
        t->next = current;
    }
    current->wait_wq = wq;
    current->timed_out = false;

    if (timeout_ms != 0) {
        current->wake_at = ticks_ms() + timeout_ms;
        current->tnext = timer_list;
        timer_list = current;
    } else {
        current->wake_at = 0;
    }
}

static void wake_task(task_t *t)
{
    if (t->wake_at != 0) {
        timer_unlink(t);
        t->wake_at = 0;
    }
    t->wait_wq = NULL;
    t->state = TASK_READY;
    sched_enqueue(t);
}

__attribute__((unused))     /* unused on x86_64: its sleeps park for real */
static void cpu_relax_hint(void)
{
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#else
    __asm__ volatile ("" : : : "memory");
#endif
}

void thread_set_priority(task_t *t, unsigned prio)
{
    if (prio > SCHED_PRIO_MAX)
        panic("thread: priority %u out of range", prio);

    irqstate_t s = irq_save();
    if (t == current) {
        t->prio = (uint8_t)prio;
        t->quantum = quantum_for(prio);
    } else if (t->state == TASK_READY) {
        /* Move it between runqueues. */
        task_t **pp = &rq_head[t->prio];
        while (*pp && *pp != t)
            pp = &(*pp)->next;
        if (*pp == NULL)
            panic("thread: READY task missing from runqueue");
        *pp = t->next;
        if (rq_tail[t->prio] == t)
            rq_tail[t->prio] = NULL;
        t->prio = (uint8_t)prio;
        sched_enqueue(t);
        t->quantum = quantum_for(prio);
    } else {
        t->prio = (uint8_t)prio;
    }
    irq_restore(s);
}

task_t *kthread_create(void (*fn)(uint64_t), uint64_t arg)
{
    task_t *t = kmalloc(sizeof *t);
    if (t == NULL)
        return NULL;
    memset(t, 0, sizeof *t);

    uint64_t *top;
    if (static_stacks_used < STATIC_STACKS) {
        top = task_stacks[static_stacks_used++] + STACK_WORDS;
    } else {
        top = (uint64_t *)kmalloc(STACK_WORDS * 8) + STACK_WORDS;
        if (top == NULL) {
            kfree(t);
            return NULL;
        }
        t->stk_base = (void *)((uint64_t)top - STACK_WORDS * 8);
        t->stk_heap = true;
    }
    t->sp = make_frame(top, (uint64_t)fn, arg);
    t->as = NULL;
    t->state = TASK_READY;
    t->prio = SCHED_PRIO_MAX;
    t->quantum = quantum_for(t->prio);
    t->name = "kthread";

    irqstate_t s = irq_save();
    sched_enqueue(t);
    irq_restore(s);

    return t;
}

void schedule(void)
{
    if (!sched_active || !current)
        return;

    for (;;) {
        /* The switch decision (requeue, pick, address-space install)
         * and switch_to itself must be atomic against the timer: a
         * preemption in between would save the physically running
         * stack into the wrong task's slot. Interrupt state travels
         * with each task's own stack. */
        irqstate_t s = irq_save();

        if (current->state == TASK_RUNNING) {
            current->state = TASK_READY;
            sched_enqueue(current);
        }

        task_t *next = sched_pick();
        if (next == NULL) {
            /* Nothing runnable: the current task is blocking or exiting.
             * Wait for the timer to wake someone. aarch64/riscv64 have
             * no timer interrupt yet, so their callers must guarantee a
             * waker exists (or poll). */
            irq_restore(s);
#if defined(__x86_64__)
            __asm__ volatile ("sti\n\thlt\n\tcli" : : : "memory");
#endif
            continue;
        }

        if (next == current) {
            /* We were the only runnable task: keep the CPU. Never
             * switch_to ourselves — kmain has no saved context (sp == 0)
             * and reloading it would drop the stack to zero. */
            current->state = TASK_RUNNING;
            irq_restore(s);
            return;
        }

        next->state = TASK_RUNNING;
        next->quantum = quantum_for(next->prio);
        task_t *prev = current;
        current = next;
        /* Install the next task's address space before it runs. Kernel
         * threads carry as == NULL and get the shared kernel root. */
        vm_as_activate(next->as);
        switch_to(&prev->sp, next->sp);
        irq_restore(s);
        return;
    }
}

void yield(void)
{
    schedule();
}

void sched_tick(void)
{
    if (!sched_active)
        return;

    /* Wake deadline-expired sleepers first. */
    uint64_t now = ticks_ms();
    task_t **pp = &timer_list;
    while (*pp != NULL) {
        task_t *t = *pp;
        if (now >= t->wake_at) {
            *pp = t->tnext;
            t->tnext = NULL;
            t->timed_out = true;
            if (t->wait_wq != NULL) {
                wq_unlink(t->wait_wq, t);
                t->wait_wq = NULL;
            }
            wake_task(t);
        } else {
            pp = &t->tnext;
        }
    }

    if (current != NULL && current->state == TASK_RUNNING) {
        current->cpu_ticks++;
        if (current->quantum > 0 && --current->quantum == 0)
            schedule();     /* quantum exhausted: pre-empt */
    }
}

void wq_init(wq_t *wq)
{
    wq->head = NULL;
}

void wq_block(wq_t *wq, spinlock_t *lk)
{
    irqstate_t s = irq_save();
    block_on(wq, 0);
    spin_unlock(lk);
    schedule();
    irq_restore(s);
}

bool wq_block_timed(wq_t *wq, uint64_t ms, spinlock_t *lk)
{
    irqstate_t s = irq_save();
    block_on(wq, ms);
    spin_unlock(lk);
    schedule();
    bool out = !current->timed_out;
    current->timed_out = false;
    irq_restore(s);
    return out;
}

void wq_wake_one(wq_t *wq)
{
    irqstate_t s = irq_save();
    task_t *t = wq->head;
    if (t != NULL) {
        wq->head = t->next;
        t->next = NULL;
        wake_task(t);
    }
    irq_restore(s);
}

void wq_wake_all(wq_t *wq)
{
    irqstate_t s = irq_save();
    while (wq->head != NULL) {
        task_t *t = wq->head;
        wq->head = t->next;
        t->next = NULL;
        wake_task(t);
    }
    irq_restore(s);
}

void thread_sleep_ms(uint64_t ms)
{
#if defined(__x86_64__)
    wq_t wq;
    wq_init(&wq);
    /* No condition lock to release: park until the deadline. */
    irqstate_t s = irq_save();
    block_on(&wq, ms);
    schedule();
    current->timed_out = false;
    irq_restore(s);
#else
    /* No timer interrupt on this architecture yet: poll. */
    uint64_t target = ticks_ms() + ms;
    while (ticks_ms() < target)
        cpu_relax_hint();
#endif
}

void thread_reap(void)
{
    irqstate_t s = irq_save();
    while (zombies != NULL) {
        task_t *t = zombies;
        zombies = t->next;
        t->next = NULL;
        bool heap = t->stk_heap;
        void *base = t->stk_base;
        if (heap)
            kfree(base);
        kfree(t);
    }
    irq_restore(s);
}

__attribute__((noreturn))
void thread_exit(void)
{
    /* Interrupts stay off across the final switch: this stack is
     * abandoned and never resumes, so there is nothing to restore. */
    irq_save();
    current->state = TASK_ZOMBIE;
    current->next = zombies;
    zombies = current;
    schedule();     /* never returns: ZOMBIE tasks are never re-picked */
    for (;;)
        ;
}
