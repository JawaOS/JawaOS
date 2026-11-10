#include <stdint.h>

#include <stdarg.h>

#include "console.h"

static void halt_forever(void) __attribute__((noreturn));

static void halt_forever(void)
{
#if defined(__x86_64__)
    __asm__ volatile ("cli");
    for (;;)
        __asm__ volatile ("hlt");
#elif defined(__aarch64__) || defined(__riscv)

    for (;;)
        __asm__ volatile ("wfi");
#else
#error "unsupported architecture for halt_forever"
#endif
}

void panic(const char *fmt, ...)
{
    va_list ap;

    kprintf("\n\n*** KERNEL PANIC ***\n");

    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\nCPU halted.\n");

    halt_forever();
}
