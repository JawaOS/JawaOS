#include <stdint.h>
#include <stddef.h>

#include "kheap.h"
#include "spinlock.h"
#include "thread.h"
#include "vm.h"

task_t *current;

#define STACK_WORDS 512
#define STATIC_STACKS 3

static bool sched_active;
static uint64_t task_stacks[STATIC_STACKS][STACK_WORDS] __attribute__((aligned(16)));
static int static_stacks_used;

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
    main_task.next = &main_task;
    main_task.sp = 0;
    main_task.as = NULL;
    current = &main_task;
    sched_active = true;
}

void kthread_create(void (*fn)(uint64_t), uint64_t arg)
{
    task_t *t = kmalloc(sizeof *t);
    uint64_t *top;
    if (static_stacks_used < STATIC_STACKS) {
        top = task_stacks[static_stacks_used++] + STACK_WORDS;
    } else {
        top = (uint64_t *)kmalloc(STACK_WORDS * 8) + STACK_WORDS;
    }
    t->sp = make_frame(top, (uint64_t)fn, arg);
    t->as = NULL;

    /* Ring insertion must not interleave with a timer-driven switch. */
    irqstate_t s = irq_save();
    t->next = current->next;
    current->next = t;
    irq_restore(s);
}

void schedule(void)
{
    if (!sched_active || !current || current->next == current)
        return;

    /* The switch decision (ring walk, current, address space install)
     * and switch_to itself must be atomic against the timer: a preemption
     * in between would save the physically running stack into the wrong
     * task's slot and turn tasks into zombies. Interrupt state travels
     * with each task's own stack: whoever is switched to later restores
     * the state it saved on its own stack. */
    irqstate_t s = irq_save();
    task_t *prev = current;
    task_t *next = current->next;
    current = next;
    /* Install the next task's address space before it runs. Kernel
     * threads carry as == NULL and get the shared kernel root. */
    vm_as_activate(next->as);
    switch_to(&prev->sp, next->sp);
    irq_restore(s);
}

void yield(void)
{
    schedule();
}

__attribute__((noreturn))
void thread_exit(void)
{
    /* Interrupts stay off across the final switch: this stack is
     * abandoned and never resumes, so there is nothing to restore. */
    irq_save();
    task_t *prev = current;
    while (prev->next != current)
        prev = prev->next;
    prev->next = current->next;
    task_t *next = current->next;
    current = next;
    vm_as_activate(next->as);
    /* The dying context must be saved into a scratch slot: saving it
     * into a neighbour's slot (prev->sp) would clobber that task's
     * saved state and turn whoever resumes it into a zombie. */
    uint64_t zombie_sp;
    switch_to(&zombie_sp, next->sp);
    for (;;)
        ;
}
