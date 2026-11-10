#include <stdint.h>

#include "console.h"
#include "spinlock.h"
#include "thread.h"
#include "timer.h"
#include "vmm.h"

#if defined(__x86_64__)

static volatile uint64_t ticks;

struct lapic {
    uint32_t r[1024];
};

static struct lapic *lapic;

#define LAPIC_ENABLE        (1u << 8)
#define LAPIC_LVT_PERIODIC  (1u << 17)
#define LAPIC_EOI           0xb0 / 4
#define LAPIC_LVT_TIMER     0x320 / 4
#define LAPIC_DIV_CONF      0x3e0 / 4
#define LAPIC_INIT_COUNT    0x380 / 4
#define LAPIC_CUR_COUNT     0x390 / 4

static inline void mmw(uint32_t *p, uint32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void timer_init(void)
{
    lapic = vmm_map_device(0xfee00000ull, 0x1000);
    mmw(&lapic->r[0xf0 / 4], 0xff | LAPIC_ENABLE);
    mmw(&lapic->r[LAPIC_DIV_CONF], 3);
    mmw(&lapic->r[LAPIC_LVT_TIMER], 32 | LAPIC_LVT_PERIODIC);
    mmw(&lapic->r[LAPIC_INIT_COUNT], 62500);
    __asm__ volatile ("sti" ::: "memory");
}

void timer_tick(void)
{
    mmw(&lapic->r[LAPIC_EOI], 0);
    ticks++;
    schedule();
}

uint64_t ticks_ms(void)
{
    return ticks;
}

void msleep(uint64_t ms)
{
    uint64_t target = ticks + ms;
    while (ticks < target)
        __asm__ volatile ("hlt");
}

#elif defined(__aarch64__)

static uint64_t freq;

static inline uint64_t cntpct(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

void timer_init(void)
{
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
}

void timer_tick(void)
{
}

uint64_t ticks_ms(void)
{
    return cntpct() / (freq / 1000);
}

void msleep(uint64_t ms)
{
    uint64_t target = cntpct() + freq / 1000 * ms;
    while (cntpct() < target)
        __asm__ volatile ("yield");
}

#elif defined(__riscv)

#define TIMEBASE 10000000ull

static volatile uint64_t ticks;
static uint64_t interval;

static inline uint64_t rdtime(void)
{
    uint64_t v;
    __asm__ volatile ("rdtime %0" : "=r"(v));
    return v;
}

void timer_init(void)
{
    interval = TIMEBASE / 1000;
}

void timer_tick(void)
{
    ticks++;
    schedule();
}

uint64_t ticks_ms(void)
{
    return rdtime() / interval;
}

void msleep(uint64_t ms)
{
    uint64_t target = rdtime() + interval * ms;
    while (rdtime() < target)
        __asm__ volatile ("addi x0, x0, 0");
}

#else
#error "unsupported architecture"
#endif
