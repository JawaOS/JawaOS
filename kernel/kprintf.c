#include <stdint.h>
#include <stddef.h>

#include <stdarg.h>

#include "console.h"

static int emit_num(unsigned long long v, unsigned base, int uppercase,
                    int zero_pad, int width, int negative)
{
    char buf[24];
    int n = 0;
    int count = 0;
    const char hex_lo[] = "0123456789abcdef";
    const char hex_up[] = "0123456789ABCDEF";

    if (v == 0)
        buf[n++] = '0';
    while (v != 0) {
        unsigned d = (unsigned)(v % base);
        buf[n++] = (d < 10) ? (char)('0' + d)
                            : (uppercase ? hex_up : hex_lo)[d];
        v /= base;
    }

    int pad = width - n - (negative ? 1 : 0);
    if (!zero_pad)
        while (pad-- > 0) { console_putchar(' '); count++; }
    if (negative) { console_putchar('-'); count++; }
    if (zero_pad)
        while (pad-- > 0) { console_putchar('0'); count++; }
    while (n > 0) {
        console_putchar(buf[--n]);
        count++;
    }
    return count;
}

enum len_modifier { LEN_INT, LEN_LONG, LEN_LLONG, LEN_SIZE };

int kvprintf(const char *fmt, va_list ap)
{
    int count = 0;
    const char *p = fmt;

    while (*p != '\0') {
        if (*p != '%') {
            console_putchar(*p++);
            count++;
            continue;
        }

        p++;

        if (*p == '%') {
            console_putchar(*p++);
            count++;
            continue;
        }

        int zero_pad = 0;
        if (*p == '0') {
            zero_pad = 1;
            p++;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');

        enum len_modifier len = LEN_INT;
        while (*p == 'l') {
            len = (len == LEN_LONG) ? LEN_LLONG : LEN_LONG;
            p++;
        }
        if (*p == 'z') {
            len = LEN_SIZE;
            p++;
        }
        while (*p == 'h')
            p++;

        switch (*p) {

        case 'c': {
            char c = (char)va_arg(ap, int);
            console_putchar(c);
            count++;
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            int n = 0;
            while (s[n] != '\0')
                n++;
            for (int pad = width - n; pad > 0; pad--) {
                console_putchar(' ');
                count++;
            }
            for (int i = 0; i < n; i++) {
                console_putchar(s[i]);
                count++;
            }
            break;
        }

        case 'd':
        case 'i': {
            long long v;
            if (len == LEN_LLONG)
                v = va_arg(ap, long long);
            else if (len == LEN_LONG || len == LEN_SIZE)
                v = va_arg(ap, long);
            else
                v = va_arg(ap, int);

            int negative = 0;
            unsigned long long u;
            if (v < 0) {
                negative = 1;
                u = (unsigned long long)(-(v + 1)) + 1;
            } else {
                u = (unsigned long long)v;
            }
            count += emit_num(u, 10, 0, zero_pad, width, negative);
            break;
        }

        case 'u':
        case 'x':
        case 'X': {
            unsigned long long u;
            if (len == LEN_LLONG)
                u = va_arg(ap, unsigned long long);
            else if (len == LEN_LONG || len == LEN_SIZE)
                u = va_arg(ap, unsigned long);
            else
                u = va_arg(ap, unsigned int);

            unsigned base = (*p == 'u') ? 10 : 16;
            count += emit_num(u, base, *p == 'X', zero_pad, width, 0);
            break;
        }

        case 'p': {
            uintptr_t ptr = (uintptr_t)va_arg(ap, void *);
            console_putchar('0');
            console_putchar('x');
            count += 2;
            count += emit_num(ptr, 16, 0, 1, 2 * (int)sizeof(uintptr_t), 0);
            break;
        }

        case '\0':
            return count;

        default:
            console_putchar('%');
            console_putchar(*p);
            count += 2;
            break;
        }

        if (*p != '\0')
            p++;
    }

    return count;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = kvprintf(fmt, ap);
    va_end(ap);
    return r;
}
