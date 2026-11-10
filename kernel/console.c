#include "console.h"

void console_init(void)
{
    serial_init();
    fbcon_init();
}

void console_putchar(char c)
{
    serial_putchar(c);
    fbcon_putchar(c);
}
