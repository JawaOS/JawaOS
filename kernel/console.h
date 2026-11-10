#ifndef JAWA_CONSOLE_H
#define JAWA_CONSOLE_H

#include <stdarg.h>

void console_init(void);
void console_putchar(char c);

void serial_init(void);
void serial_putchar(char c);

void fbcon_init(void);
void fbcon_putchar(char c);

int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char *fmt, va_list ap);

void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif
