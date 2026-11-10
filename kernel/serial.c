#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "console.h"
#include "pmm.h"
#include "vmm.h"

#if defined(__x86_64__)

#define COM1 0x3f8

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %w1, %b0" : "=a"(val) : "Nd"(port));
    return val;
}

static void serial_putc_raw(char c)
{
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

static void serial_port_init(void)
{

    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static bool serial_port_ready(void)
{
    return true;
}

#elif defined(__aarch64__)

#define PL011_PHYS 0x09000000UL

static volatile uint32_t *pl011;

static void serial_putc_raw(char c)
{
    while (pl011[0x18 / 4] & (1u << 5))
        ;
    pl011[0x00 / 4] = (uint32_t)(uint8_t)c;
}

static void serial_port_init(void)
{

    pl011 = vmm_map_device(PL011_PHYS, PAGE_SIZE);

    pl011[0x44 / 4] = 0x7FF;
    pl011[0x24 / 4] = 13;
    pl011[0x28 / 4] = 1;
    pl011[0x2C / 4] = (3u << 5) | (1u << 4);
    pl011[0x30 / 4] = (1u << 0) | (1u << 8) | (1u << 9);
}

static bool serial_port_ready(void)
{
    return pl011 != NULL;
}

#elif defined(__riscv)

#define UART0_PHYS 0x10000000UL

static volatile uint8_t *uart;

static void serial_putc_raw(char c)
{
    while (!(uart[5] & 0x20))
        ;
    uart[0] = (uint8_t)c;
}

static void serial_port_init(void)
{

    uart = vmm_map_device(UART0_PHYS, PAGE_SIZE);

    uart[1] = 0x00;
    uart[3] = 0x80;
    uart[0] = 0x01;
    uart[1] = 0x00;
    uart[3] = 0x03;
    uart[2] = 0x07;
}

static bool serial_port_ready(void)
{
    return uart != NULL;
}

#else
#error "unsupported architecture for serial console"
#endif

void serial_init(void)
{
    serial_port_init();
}

void serial_putchar(char c)
{
    if (!serial_port_ready())
        return;
    if (c == '\n')
        serial_putc_raw('\r');
    serial_putc_raw(c);
}
