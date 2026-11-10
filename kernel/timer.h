#ifndef JAWA_TIMER_H
#define JAWA_TIMER_H

#include <stdint.h>

void timer_init(void);
void timer_tick(void);
uint64_t ticks_ms(void);
void msleep(uint64_t ms);

#endif
