#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "console.h"
#include "kheap.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"
#include "vmm.h"

#define POISON_ALLOC KHEAP_POISON_ALLOC
#define POISON_FREE  KHEAP_POISON_FREE

#define KHEAP_ALIGN     16
#define KHEAP_HDR_SIZE  16
#define KHEAP_MIN_BLOCK (KHEAP_HDR_SIZE + KHEAP_ALIGN)

#define MAGIC_ALLOC 0xA110CA7Eu
#define MAGIC_FREE  0xF7EE810Cu

struct kblock {
    uint64_t size;
    uint32_t magic;
    uint32_t _reserved;
};

struct fblock {
    uint64_t size;
    uint32_t magic;
    uint32_t _reserved;
    struct fblock *next;
};

static struct fblock *free_list;
static spinlock_t heap_lock = SPINLOCK_INIT;

static uint64_t heap_top = KHEAP_BASE;
static uint64_t heap_allocated;
static uint64_t heap_mapped;

static void kheap_grow(size_t min_bytes)
{
    size_t need = min_bytes + KHEAP_HDR_SIZE;
    uint64_t npages = (need + PAGE_MASK) >> PAGE_SHIFT;

    if (heap_top + npages * PAGE_SIZE > KHEAP_BASE + KHEAP_MAX_SIZE)
        panic("kheap: 64 MiB heap exhausted");

    uint64_t va = heap_top;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t frame = pmm_alloc();
        if (frame == 0)
            panic("kheap: PMM exhausted while growing the heap");
        vm_map(va + i * PAGE_SIZE, frame, VM_PROT_READ | VM_PROT_WRITE);
    }

    struct fblock *nb = (struct fblock *)va;
    nb->size = npages * PAGE_SIZE;
    nb->magic = MAGIC_FREE;

    struct fblock **pp = &free_list;
    while (*pp != NULL && (uint64_t)*pp < va)
        pp = &(*pp)->next;
    nb->next = *pp;
    *pp = nb;

    heap_top += npages * PAGE_SIZE;
    heap_mapped += npages * PAGE_SIZE;
}

static void kheap_free_insert(struct fblock *blk)
{
    struct fblock **pp = &free_list;

    while (*pp != NULL && (uint64_t)*pp < (uint64_t)blk)
        pp = &(*pp)->next;

    if (*pp != NULL && (uint64_t)blk + blk->size == (uint64_t)*pp) {
        blk->size += (*pp)->size;
        blk->next = (*pp)->next;
    } else {
        blk->next = *pp;
    }

    if (pp != &free_list) {
        struct fblock *pred = (struct fblock *)
            ((char *)pp - offsetof(struct fblock, next));
        if ((uint64_t)pred + pred->size == (uint64_t)blk) {
            pred->size += blk->size;
            pred->next = blk->next;
            return;
        }
    }

    *pp = blk;
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size_t need = (size + KHEAP_HDR_SIZE + KHEAP_ALIGN - 1) &
                  ~(size_t)(KHEAP_ALIGN - 1);

    spin_lock(&heap_lock);

    struct fblock **pp = &free_list;
    while (*pp != NULL) {
        struct fblock *fb = *pp;

        if (fb->size >= need + KHEAP_MIN_BLOCK) {

            struct kblock *kb = (struct kblock *)fb;
            size_t rest = fb->size - need;
            struct fblock *nb = (struct fblock *)((uint64_t)fb + need);
            nb->size = rest;
            nb->magic = MAGIC_FREE;
            nb->next = fb->next;
            *pp = nb;

            kb->size = need;
            kb->magic = MAGIC_ALLOC;
            heap_allocated += need;
            spin_unlock(&heap_lock);

            void *payload = (void *)((uint64_t)kb + KHEAP_HDR_SIZE);
#ifdef JAWA_DEBUG
            memset(payload, POISON_ALLOC, need - KHEAP_HDR_SIZE);
#endif
            return payload;
        }

        if (fb->size >= need) {

            struct kblock *kb = (struct kblock *)fb;
            *pp = fb->next;
            kb->size = fb->size;
            kb->magic = MAGIC_ALLOC;
            heap_allocated += fb->size;
            spin_unlock(&heap_lock);

            void *payload = (void *)((uint64_t)kb + KHEAP_HDR_SIZE);
#ifdef JAWA_DEBUG
            memset(payload, POISON_ALLOC, fb->size - KHEAP_HDR_SIZE);
#endif
            return payload;
        }

        pp = &fb->next;
    }

    kheap_grow(need);
    spin_unlock(&heap_lock);
    return kmalloc(size);
}

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    uint64_t addr = (uint64_t)ptr;

    if (addr < KHEAP_BASE + KHEAP_HDR_SIZE || addr >= heap_top)
        panic("kheap: kfree of pointer 0x%llx outside the heap",
              (unsigned long long)addr);
    if ((addr & (KHEAP_ALIGN - 1)) != 0)
        panic("kheap: kfree of unaligned pointer 0x%llx",
              (unsigned long long)addr);

    spin_lock(&heap_lock);

    struct kblock *kb = (struct kblock *)(addr - KHEAP_HDR_SIZE);
    if (kb->magic != MAGIC_ALLOC)
        panic("kheap: kfree of bad/double-freed block at 0x%llx "
              "(magic 0x%x)", (unsigned long long)addr, kb->magic);

#ifdef JAWA_DEBUG
    memset(ptr, POISON_FREE, kb->size - KHEAP_HDR_SIZE);
#endif

    heap_allocated -= kb->size;

    struct fblock *fb = (struct fblock *)kb;
    fb->magic = MAGIC_FREE;
    kheap_free_insert(fb);

    spin_unlock(&heap_lock);
}

void kheap_init(void)
{

}

void kheap_stats(uint64_t *allocated_bytes, uint64_t *mapped_bytes)
{
    spin_lock(&heap_lock);
    if (allocated_bytes)
        *allocated_bytes = heap_allocated;
    if (mapped_bytes)
        *mapped_bytes = heap_mapped;
    spin_unlock(&heap_lock);
}
