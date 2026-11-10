#include <stdint.h>
#include <stddef.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"

struct freepage {
    struct freepage *next;
};

#define POISON_ALLOC 0xCC
#define POISON_FREE  0xDD

#define BOOTMEM_PAGES 4

static struct freepage *free_list;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static uint64_t total_usable_pages;
static uint64_t free_pages;

static uint64_t bootmem_base;
static uint64_t bootmem_pages_left;

#ifdef JAWA_DEBUG
static bool pmm_in_usable(uint64_t phys)
{
    for (uint64_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct boot_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        if (phys >= e->base && phys + PAGE_SIZE <= e->base + e->length)
            return true;
    }
    return false;
}
#endif

void pmm_init(void)
{
    if (bootinfo.memmap_count == 0)
        panic("pmm: no memory map entries");

    uint64_t best = 0, best_pages = 0;
    for (uint64_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct boot_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t pages = page_align_down(e->length) >> PAGE_SHIFT;
        if (pages > best_pages) {
            best_pages = pages;
            best = i;
        }
    }
    if (best_pages < BOOTMEM_PAGES + 1)
        panic("pmm: no USABLE region large enough for the bootmem pool");

    const struct boot_memmap_entry *r = &bootinfo.memmap[best];
    bootmem_base = (r->base + 0x3FFF) & ~0x3FFFULL;
    if (bootmem_base + BOOTMEM_PAGES * PAGE_SIZE > r->base + r->length)
        panic("pmm: bootmem carve does not fit");
    bootmem_pages_left = BOOTMEM_PAGES;

    for (uint64_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct boot_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t base = page_align_up(e->base);
        uint64_t end = page_align_down(e->base + e->length);

        if (i == best)
            base = bootmem_base + BOOTMEM_PAGES * PAGE_SIZE;

        if (end <= base)
            continue;
        total_usable_pages += (end - base) >> PAGE_SHIFT;

        for (uint64_t p = end; p >= base + PAGE_SIZE; p -= PAGE_SIZE) {
            struct freepage *fp =
                (struct freepage *)boot_phys_to_virt(p - PAGE_SIZE);
            fp->next = free_list;
            free_list = fp;
            free_pages++;
        }
    }

    if (free_list == NULL)
        panic("pmm: no free pages after init");
}

uint64_t pmm_alloc(void)
{
    spin_lock(&pmm_lock);

    struct freepage *fp = free_list;
    if (fp == NULL) {
        spin_unlock(&pmm_lock);
        return 0;
    }
    free_list = fp->next;
    free_pages--;

    spin_unlock(&pmm_lock);

    uint64_t phys = boot_virt_to_phys((uint64_t)fp);

#ifdef JAWA_DEBUG
    memset((void *)fp, POISON_ALLOC, PAGE_SIZE);
#endif

    return phys;
}

void pmm_free(uint64_t phys)
{
    if (!page_aligned(phys) || phys == 0)
        panic("pmm: pmm_free of misaligned/zero address 0x%llx",
              (unsigned long long)phys);

#ifdef JAWA_DEBUG
    if (!pmm_in_usable(phys))
        panic("pmm: pmm_free of non-USABLE page 0x%llx",
              (unsigned long long)phys);
    memset((void *)boot_phys_to_virt(phys), POISON_FREE, PAGE_SIZE);
#endif

    struct freepage *fp = (struct freepage *)boot_phys_to_virt(phys);

    spin_lock(&pmm_lock);
    fp->next = free_list;
    free_list = fp;
    free_pages++;
    spin_unlock(&pmm_lock);
}

uint64_t pmm_alloc_range(uint64_t npages)
{
    if (npages == 0)
        return 0;

    spin_lock(&pmm_lock);

    if (npages > free_pages) {
        spin_unlock(&pmm_lock);
        return 0;
    }

    uint64_t base_phys = boot_virt_to_phys((uint64_t)free_list);
    uint64_t prev_phys = base_phys;
    uint64_t walked = 1;
    struct freepage *fp = free_list->next;

    while (walked < npages) {
        if (fp == NULL)
            goto fail;
        uint64_t phys = boot_virt_to_phys((uint64_t)fp);
        if (phys != prev_phys + PAGE_SIZE)
            goto fail;
        prev_phys = phys;
        walked++;
        fp = fp->next;
    }

    free_list = fp;
    free_pages -= npages;

    spin_unlock(&pmm_lock);

#ifdef JAWA_DEBUG
    for (uint64_t p = base_phys; p < base_phys + npages * PAGE_SIZE;
         p += PAGE_SIZE) {
        if (!pmm_in_usable(p))
            panic("pmm: pmm_alloc_range covers non-USABLE page 0x%llx",
                  (unsigned long long)p);
        memset((void *)boot_phys_to_virt(p), POISON_ALLOC, PAGE_SIZE);
    }
#endif

    return base_phys;

fail:
    spin_unlock(&pmm_lock);
    return 0;
}

uint64_t bootmem_alloc(uint64_t npages)
{
    if (npages > bootmem_pages_left)
        panic("pmm: bootmem pool exhausted (%u pages requested)",
              (unsigned)npages);

    uint64_t phys = bootmem_base;
    bootmem_base += npages * PAGE_SIZE;
    bootmem_pages_left -= npages;

    memset((void *)boot_phys_to_virt(phys), 0, npages * PAGE_SIZE);
    return phys;
}

uint64_t pmm_total_usable_pages(void)
{
    return total_usable_pages;
}

uint64_t pmm_free_pages(void)
{
    return free_pages;
}
