#include <stddef.h>

#include "console.h"

#include "spinlock.h"
#include "thread.h"

/* Single-CPU console lock: output (glyph drawing, scroll redraw) must be
 * atomic against preemption — with the preemptive scheduler, a task
 * switched out mid-scroll left a half-copied screen, and two tasks
 * printing interleaved glyphs at the same cursor position.
 *
 * The lock is always held with interrupts disabled, so a timer ISR can
 * never fire (and try to print) while another task owns the lock.
 * Re-entrant acquisition is allowed for the SAME task: an exception
 * taken while the owner prints (e.g. a demand-page fault in the middle
 * of a scroll redraw) must be able to print its report without
 * deadlocking on the owner's own lock. Early boot runs before
 * thread_init(), where current is still NULL — NULL self-matches the
 * owner slot and behaves as recursion, which is correct there. */

static task_t *con_owner;
static int con_depth;

static inline void cpu_relax(void)
{
#if defined(__x86_64__)
    __asm__ volatile ("pause");
#elif defined(__aarch64__)
    __asm__ volatile ("yield");
#else
    __asm__ volatile ("" : : : "memory");
#endif
}

void console_init(void)
{
    serial_init();
    fbcon_init();
}

void console_putchar(char c)
{
    irqstate_t s = irq_save();

    if (con_owner != current) {
        while (con_owner != NULL)
            cpu_relax();
        con_owner = current;
        con_depth = 0;
    }
    con_depth++;

    serial_putchar(c);
    fbcon_putchar(c);

    if (--con_depth == 0)
        con_owner = NULL;

    irq_restore(s);
}
