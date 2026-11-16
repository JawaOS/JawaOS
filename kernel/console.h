#ifndef JAWA_CONSOLE_H
#define JAWA_CONSOLE_H

#include <stdarg.h>

void console_init(void);
void console_putchar(char c);
/* Batched output: one lock acquisition, one FIFO-paced serial pass and a
 * single deferred scroll flush per call. */
void console_write(const char *buf, unsigned long n);
/* Hold the console lock across a formatted print so kprintf can buffer
 * internally; console_write() called inside the hold re-enters safely. */
void console_lock_begin(void);
void console_lock_end(void);

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *buf, unsigned long n);

void fbcon_init(void);
void fbcon_putchar(char c);
void fbcon_write(const char *buf, unsigned long n);
void fbcon_flush(void);

int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char *fmt, va_list ap);

void panic(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

#endif
