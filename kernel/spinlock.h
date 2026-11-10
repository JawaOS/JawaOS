#ifndef JAWA_SPINLOCK_H
#define JAWA_SPINLOCK_H

#include <stdbool.h>

typedef struct {
    volatile unsigned char locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

void spin_lock(spinlock_t *lk);
void spin_unlock(spinlock_t *lk);
bool spin_trylock(spinlock_t *lk);

typedef unsigned long irqstate_t;

irqstate_t irq_save(void);
void irq_restore(irqstate_t state);

static inline irqstate_t spin_lock_irqsave(spinlock_t *lk)
{
    irqstate_t s = irq_save();
    spin_lock(lk);
    return s;
}

static inline void spin_unlock_irqrestore(spinlock_t *lk, irqstate_t s)
{
    spin_unlock(lk);
    irq_restore(s);
}

#endif
