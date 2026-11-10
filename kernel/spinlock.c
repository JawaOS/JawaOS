#include "spinlock.h"

#include <stdint.h>

static inline void cpu_relax(void)
{
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#else

    __asm__ volatile ("");
#endif
}

void spin_lock(spinlock_t *lk)
{
    for (;;) {
        if (!__atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE))
            return;
        while (__atomic_load_n(&lk->locked, __ATOMIC_RELAXED))
            cpu_relax();
    }
}

void spin_unlock(spinlock_t *lk)
{
    __atomic_clear(&lk->locked, __ATOMIC_RELEASE);
}

bool spin_trylock(spinlock_t *lk)
{
    return !__atomic_test_and_set(&lk->locked, __ATOMIC_ACQUIRE);
}

#if defined(__x86_64__)

irqstate_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return (irqstate_t)((flags >> 9) & 1);
}

void irq_restore(irqstate_t state)
{
    if (state)
        __asm__ volatile ("sti" ::: "memory");
}

#elif defined(__aarch64__)

irqstate_t irq_save(void)
{
    uint64_t daif;
    __asm__ volatile ("mrs %0, daif\n\tmsr daifset, #2"
                      : "=r"(daif) :: "memory");
    return (irqstate_t)(((daif >> 7) & 1) ^ 1);
}

void irq_restore(irqstate_t state)
{
    if (state)
        __asm__ volatile ("msr daifclr, #2" ::: "memory");
}

#elif defined(__riscv)

#define SSTATUS_SIE ((uint64_t)1 << 1)

irqstate_t irq_save(void)
{
    uint64_t sstatus;
    __asm__ volatile ("csrr %0, sstatus\n\tcsrc sstatus, %1"
                      : "=r"(sstatus) : "r"(SSTATUS_SIE) : "memory");
    return (irqstate_t)((sstatus >> 1) & 1);
}

void irq_restore(irqstate_t state)
{
    if (state)
        __asm__ volatile ("csrs sstatus, %0" :: "r"(SSTATUS_SIE) : "memory");
}

#else
#error "unsupported architecture for irq_save/irq_restore"
#endif
