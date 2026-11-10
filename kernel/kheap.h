#ifndef JAWA_KHEAP_H
#define JAWA_KHEAP_H

#include <stddef.h>
#include <stdint.h>

#define KHEAP_POISON_ALLOC 0xAA
#define KHEAP_POISON_FREE  0xDE

void kheap_init(void);

void *kmalloc(size_t size);
void kfree(void *ptr);

void kheap_stats(uint64_t *allocated_bytes, uint64_t *mapped_bytes);

#endif
