#ifndef JAWA_PMM_H
#define JAWA_PMM_H

#include <stdint.h>
#include <stdbool.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1ull << PAGE_SHIFT)
#define PAGE_MASK (PAGE_SIZE - 1)

static inline uint64_t page_align_down(uint64_t x) { return x & ~PAGE_MASK; }
static inline uint64_t page_align_up(uint64_t x)   { return (x + PAGE_MASK) & ~PAGE_MASK; }
static inline bool     page_aligned(uint64_t x)    { return (x & PAGE_MASK) == 0; }

void pmm_init(void);

uint64_t pmm_alloc(void);

void pmm_free(uint64_t phys);

uint64_t bootmem_alloc(uint64_t npages);

uint64_t pmm_alloc_range(uint64_t npages);

uint64_t pmm_total_usable_pages(void);
uint64_t pmm_free_pages(void);

#endif
