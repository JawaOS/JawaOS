#ifndef JAWA_INT_H
#define JAWA_INT_H

#include <stdint.h>
#include <stdbool.h>

void int_init(void);

void int_dump_frame(const void *tf);

const char *int_exception_name(uint64_t vector);

bool int_breakpoint_selftest(void);

bool fault_probe(uint64_t addr);

void int_panic_dump(const void *tf, uint64_t vector, uint64_t detail,
                    uint64_t fault_addr);

#endif
