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
static irqstate_t con_irq;

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

void console_lock_begin(void)
{
    if (con_owner != current) {
        con_irq = irq_save();
        while (con_owner != NULL)
            cpu_relax();
        con_owner = current;
        con_depth = 0;
    }
    con_depth++;
}

void console_lock_end(void)
{
    if (--con_depth == 0) {
        con_owner = NULL;
        irq_restore(con_irq);
    }
}

/* One acquisition per burst: batched FIFO-paced serial pass, then the
 * framebuffer pass. The scroll redraw is NOT flushed here — glyph and
 * grid updates cost microseconds per line, while a full-screen repaint
 * costs about a millisecond — so the screen catches up at task-switch
 * cadence (see schedule()) instead of once per printed line. */
void console_write(const char *buf, unsigned long n)
{
    if (n == 0)
        return;

    console_lock_begin();

    serial_write(buf, n);
    fbcon_write(buf, n);

    console_lock_end();
}

void console_putchar(char c)
{
    console_write(&c, 1);
}
