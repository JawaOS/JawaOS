#include <stdint.h>
#include <stddef.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "kheap.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"
#include "vm.h"
#include "vmm.h"

struct vm_vma {
    uint64_t start;         /* first mapped byte  */
    uint64_t end;           /* one past the last  */
    unsigned flags;         /* VM_PROT_* | VM_VMA_* */
    struct vm_vma *next;    /* sorted by start, non-overlapping */
};

struct vm_as {
    uint64_t root_pa;
    uint64_t table_pages;
    uint64_t resident_pages;
    struct vm_vma *vmas;    /* sorted list of mapped regions */
    spinlock_t lock;
};

/* This CPU's active user address space (NULL = kernel root). A per-CPU
 * variable once SMP bring-up lands; the shootdown layer is built around
 * that model already. */
static vm_as_t *active_as;

/* ---- shared-frame refcounts (copy-on-write) ----
 *
 * xv6-cow-lab style: a direct table indexed by PFN, sized from the boot
 * memory map. Entry 0 means "untracked" — frames that were never shared
 * (kheap pages, page tables, plain user frames) are freed directly by
 * their owners; once a clone shares a frame it becomes tracked.
 * The clone registers the first sharing as 2 (parent + child). */
#define VM_VMA_COW 0x1000u      /* region downgraded at clone time */

static uint32_t *frame_refs;
static uint64_t frame_refs_max;
static spinlock_t frame_refs_lock = SPINLOCK_INIT;

static void frame_refs_init(void)
{
    if (frame_refs != NULL)
        return;

    /* The PMM only ever hands out USABLE frames, so size the table by
     * the top of the highest USABLE region — not the whole physical
     * address space (reserved MMIO windows can reach into the 64-bit
     * range and would make this table absurdly large). */
    uint64_t max_phys = 0;
    for (uint64_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct boot_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t top = page_align_up(e->base + e->length);
        if (top > max_phys)
            max_phys = top;
    }
    frame_refs_max = max_phys >> PAGE_SHIFT;

    frame_refs = kmalloc((frame_refs_max + 1) * sizeof(uint32_t));
    if (frame_refs == NULL)
        panic("vm: cannot allocate frame refcount table");
    memset(frame_refs, 0, (frame_refs_max + 1) * sizeof(uint32_t));
}

static uint32_t cow_refs(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    return pfn <= frame_refs_max ? frame_refs[pfn] : 0;
}

static void cow_ref_inc(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn > frame_refs_max)
        panic("vm: COW refcount for out-of-range frame 0x%llx",
              (unsigned long long)pa);

    spin_lock(&frame_refs_lock);
    frame_refs[pfn] = frame_refs[pfn] == 0 ? 2 : frame_refs[pfn] + 1;
    spin_unlock(&frame_refs_lock);
}

/* Returns true when the last reference disappeared and the frame was
 * released to the PMM. */
static bool cow_ref_dec(uint64_t pa)
{
    uint64_t pfn = pa >> PAGE_SHIFT;
    if (pfn > frame_refs_max)
        panic("vm: COW refcount for out-of-range frame 0x%llx",
              (unsigned long long)pa);

    spin_lock(&frame_refs_lock);
    if (frame_refs[pfn] == 0) {
        spin_unlock(&frame_refs_lock);
        panic("vm: COW refcount underflow on frame 0x%llx",
              (unsigned long long)pa);
    }
    frame_refs[pfn]--;
    bool gone = frame_refs[pfn] == 0;
    spin_unlock(&frame_refs_lock);

    if (gone)
        pmm_free(pa);
    return gone;
}

/* aarch64: all-zero root installed whenever no user AS is active, so
 * every access to the low VA half deterministically faults. */
#if defined(__aarch64__)
static uint64_t a64_null_root;
#endif

vm_as_t *vm_as_current(void)
{
    return active_as;
}

uint64_t vm_as_root(vm_as_t *as)
{
    return as ? as->root_pa : 0;
}

uint64_t vm_as_table_pages(vm_as_t *as)
{
    return as ? as->table_pages : 0;
}

uint64_t vm_as_resident_pages(vm_as_t *as)
{
    return as ? as->resident_pages : 0;
}

/* ---- VMA bookkeeping ---- */

static struct vm_vma *vma_find(struct vm_as *as, uint64_t va)
{
    for (struct vm_vma *v = as->vmas; v; v = v->next)
        if (va >= v->start && va < v->end)
            return v;
    return NULL;
}

/* First-fit gap of `len` bytes above `from`, below `limit`. */
static uint64_t vma_first_fit(struct vm_as *as, uint64_t from, uint64_t len,
                              uint64_t limit)
{
    uint64_t cursor = from;

    for (struct vm_vma *v = as->vmas; v; v = v->next) {
        if (v->start >= cursor + len)
            return cursor;
        if (v->end > cursor)
            cursor = v->end;
        if (cursor >= limit || cursor + len > limit)
            return 0;
    }
    return cursor + len <= limit ? cursor : 0;
}

static void vma_insert(struct vm_as *as, struct vm_vma *nv)
{
    struct vm_vma **pp = &as->vmas;

    while (*pp && (*pp)->start < nv->start)
        pp = &(*pp)->next;

    if (*pp && (*pp)->start < nv->end)
        panic("vm: VMA overlap on insert (0x%llx..0x%llx vs 0x%llx)",
              (unsigned long long)nv->start, (unsigned long long)nv->end,
              (unsigned long long)(*pp)->start);

    /* Merge with neighbours carrying identical flags. */
    if (*pp && (*pp)->start == nv->end && (*pp)->flags == nv->flags) {
        nv->end = (*pp)->end;
        nv->next = (*pp)->next;
    } else {
        nv->next = *pp;
    }
    if (pp != &as->vmas) {
        struct vm_vma *pred = (struct vm_vma *)
            ((char *)pp - offsetof(struct vm_vma, next));
        if (pred->end == nv->start && pred->flags == nv->flags) {
            pred->end = nv->end;
            pred->next = nv->next;
            return;
        }
    }
    *pp = nv;
}

/* True when [base, base+len) intersects no existing VMA. */
static bool vma_range_free(struct vm_as *as, uint64_t base, uint64_t len)
{
    for (struct vm_vma *v = as->vmas; v; v = v->next) {
        if (v->start >= base + len)
            return true;
        if (base < v->end)
            return false;
    }
    return true;
}

/* Map one page into any root (active or not). Returns the frame. */
static uint64_t vm_map_page(struct vm_as *as, uint64_t va, unsigned flags)
{
    uint64_t frame = pmm_alloc();
    if (frame == 0)
        return 0;
    memset((void *)boot_phys_to_virt(frame), 0, PAGE_SIZE);

    unsigned level;
    unsigned created = 0;
    uint64_t *slot = vmm_pte_slot(as->root_pa, va, true, &level, &created);
    as->table_pages += created;
    if (level != 1) {
        pmm_free(frame);
        return 0;
    }
    vmm_pte_write_leaf(slot, frame, flags);
    as->resident_pages++;

    /* A non-active address space has no TLB entries on any CPU yet. */
    if (as == active_as)
        vmm_tlb_inval(va);

    return frame;
}

/* Unmap one page from any root; frees the frame if it was resident.
 * Shared (COW-tracked) frames are released through the refcount. */
static bool vm_unmap_page(struct vm_as *as, uint64_t va)
{
    unsigned level;
    uint64_t *slot = vmm_pte_slot(as->root_pa, va, false, &level, NULL);
    if (slot == NULL || level != 1 || !vmm_pte_present(*slot))
        return false;

    uint64_t pa = vmm_pte_paddr(*slot);
    if (cow_refs(pa) > 0)
        cow_ref_dec(pa);
    else
        pmm_free(pa);
    as->resident_pages--;
    vmm_pte_clear(slot);

    if (as == active_as)
        vmm_tlb_inval(va);

    return true;
}

/* ---- mmap / munmap ---- */

uint64_t vm_mmap(vm_as_t *as, uint64_t hint, uint64_t len,
                 unsigned prot, unsigned vflags)
{
    if (as == NULL || len == 0)
        return 0;
    len = page_align_up(len);
    if ((prot & VM_PROT_READ) == 0 ||
        (prot & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
            (VM_PROT_WRITE | VM_PROT_EXEC))
        return 0;
    if (hint != 0 && !page_aligned(hint))
        return 0;

    uint64_t limit = vmm_user_va_limit();
    uint64_t base;

    spin_lock(&as->lock);

    if (hint != 0) {
        base = hint;
        if (base < VM_USER_BASE || base + len > limit)
            goto fail;
        if (vflags & VM_MAP_FIXED) {
            if (!vma_range_free(as, base, len))
                goto fail;
        } else {
            /* Preferred only: nearest free gap at or above the hint. */
            base = vma_first_fit(as, hint, len, limit);
            if (base == 0)
                goto fail;
        }
    } else {
        base = vma_first_fit(as, VM_USER_BASE, len, limit);
        if (base == 0)
            goto fail;
    }

    struct vm_vma *v = kmalloc(sizeof *v);
    if (v == NULL)
        goto fail;
    v->start = base;
    v->end = base + len;
    v->flags = prot | vflags;

    /* Map first, insert second: an OOM unwind then only has to release
     * this call's own pages, with no half-merged VMA in the list. */
    if (vflags & VM_MAP_POPULATE) {
        for (uint64_t va = base; va < v->end; va += PAGE_SIZE) {
            if (vm_map_page(as, va, prot) == 0) {
                for (uint64_t back = base; back < va; back += PAGE_SIZE)
                    vm_unmap_page(as, back);
                kfree(v);
                goto fail;
            }
        }
    }

    vma_insert(as, v);

    spin_unlock(&as->lock);
    return base;

fail:
    spin_unlock(&as->lock);
    return 0;
}

bool vm_munmap(struct vm_as *as, uint64_t addr, uint64_t len)
{
    if (as == NULL || len == 0 || !page_aligned(addr))
        return false;

    uint64_t end = page_align_up(addr + len);
    bool removed_anything = false;
    struct vm_vma **pp;

    spin_lock(&as->lock);

    pp = &as->vmas;
    while (*pp && (*pp)->start < end) {
        struct vm_vma *v = *pp;

        if (v->end <= addr) {           /* entirely below the range */
            pp = &v->next;
            continue;
        }

        uint64_t keep_lo = v->start < addr ? v->start : 0;
        uint64_t keep_hi = v->end > end ? v->end : 0;
        removed_anything = true;

        /* Release the resident pages inside [max(start,addr), min(end)). */
        for (uint64_t va = v->start > addr ? v->start : addr;
             va < (v->end < end ? v->end : end); va += PAGE_SIZE)
            vm_unmap_page(as, va);

        if (keep_lo && keep_hi) {
            /* Split: shrink v to the low half, insert a copy for the top. */
            uint64_t split_end = v->end;
            v->end = addr;

            struct vm_vma *hi = kmalloc(sizeof *hi);
            if (hi == NULL)
                panic("vm: OOM splitting a VMA during munmap");
            hi->start = end;
            hi->end = split_end;
            hi->flags = v->flags;
            hi->next = v->next;
            v->next = hi;
            pp = &hi->next;
        } else if (keep_lo) {
            v->end = addr;
            pp = &v->next;
        } else if (keep_hi) {
            v->start = end;
            pp = &v->next;
        } else {
            *pp = v->next;
            kfree(v);
        }
    }

    spin_unlock(&as->lock);
    return removed_anything;
}

uint64_t vm_alloc_stack(vm_as_t *as, uint64_t npages, uint64_t hint)
{
    if (as == NULL || npages == 0)
        return 0;
    if (hint != 0 && !page_aligned(hint))
        return 0;

    uint64_t limit = vmm_user_va_limit();
    if (hint != 0 &&
        (hint < VM_USER_BASE ||
         hint + (npages + 2) * PAGE_SIZE > limit))
        return 0;
    if (hint == 0) {
        hint = vma_first_fit(as, VM_USER_BASE, (npages + 2) * PAGE_SIZE,
                             limit);
        if (hint == 0)
            return 0;
    }

    /* Lower guard: registered but access-rejected by VM_VMA_GUARD no
     * matter what permissions ride along. */
    if (vm_mmap(as, hint, PAGE_SIZE, VM_PROT_READ,
                VM_VMA_GUARD | VM_MAP_FIXED) == 0)
        return 0;

    uint64_t body = vm_mmap(as, hint + PAGE_SIZE, npages * PAGE_SIZE,
                            VM_PROT_READ | VM_PROT_WRITE, VM_MAP_FIXED);
    if (body == 0) {
        vm_munmap(as, hint, PAGE_SIZE);
        return 0;
    }

    if (vm_mmap(as, body + npages * PAGE_SIZE, PAGE_SIZE, VM_PROT_READ,
                VM_VMA_GUARD | VM_MAP_FIXED) == 0) {
        vm_munmap(as, body, npages * PAGE_SIZE);
        vm_munmap(as, hint, PAGE_SIZE);
        return 0;
    }

    return body;
}

#if defined(__x86_64__)

static inline uint64_t arch_cur_root(void)
{
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & ~PAGE_MASK;
}

static void arch_set_root(uint64_t root_pa)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(root_pa) : "memory");
}

#elif defined(__aarch64__)

static void arch_set_root(uint64_t root_pa)
{
    __asm__ volatile (
        "msr ttbr0_el1, %0\n"
        "dsb sy\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb"
        : : "r"(root_pa) : "memory");
}

static inline uint64_t arch_cur_root(void)
{
    uint64_t v;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(v));
    return v & ~PAGE_MASK;
}

#elif defined(__riscv)

static inline uint64_t arch_cur_root(void)
{
    uint64_t satp;
    __asm__ volatile ("csrr %0, satp" : "=r"(satp));
    return (satp & ((1ull << 44) - 1)) << PAGE_SHIFT;
}

static void arch_set_root(uint64_t root_pa)
{
    uint64_t satp;
    __asm__ volatile ("csrr %0, satp" : "=r"(satp));
    uint64_t new_satp = (satp & (0xFull << 60)) | (root_pa >> PAGE_SHIFT);

    __asm__ volatile (
        "fence rw, rw\n"
        "csrw satp, %0\n"
        "sfence.vma zero, zero"
        : : "r"(new_satp) : "memory");
}

#else
#error "unsupported architecture for vm"
#endif

/* ---- page faults: demand paging and recovery ---- */

struct vm_fault_info vm_last_fault;

uint64_t vm_cow_copies;     /* diagnostics/tests: COW copies performed */

static bool vma_access_ok(struct vm_vma *v, bool write, bool exec)
{
    if (v->flags & VM_VMA_GUARD)
        return false;
    if (write && !(v->flags & VM_PROT_WRITE))
        return false;
    if (exec && !(v->flags & VM_PROT_EXEC))
        return false;
    return true;
}

void vm_fault_record(uint64_t va, bool write, bool exec)
{
    vm_last_fault.valid = true;
    vm_last_fault.va = va;
    vm_last_fault.write = write;
    vm_last_fault.exec = exec;

    vm_as_t *as = vm_as_current();
    struct vm_vma *v = as ? vma_find(as, page_align_down(va)) : NULL;
    vm_last_fault.guard = v != NULL && (v->flags & VM_VMA_GUARD) != 0;
}

bool vm_handle_fault(uint64_t va, bool write, bool exec)
{
    vm_as_t *as = vm_as_current();
    if (as == NULL)
        return false;
    va = page_align_down(va);

    spin_lock(&as->lock);

    struct vm_vma *v = vma_find(as, va);
    if (v == NULL || !vma_access_ok(v, write, exec)) {
        spin_unlock(&as->lock);
        return false;
    }

    unsigned level;
    uint64_t *slot = vmm_pte_slot(as->root_pa, va, false, &level, NULL);

    /* Copy-on-write: the page is resident but was downgraded to
     * read-only when the address space was cloned, and the VMA still
     * grants write. */
    if (write && slot != NULL && level == 1 && vmm_pte_present(*slot) &&
        (v->flags & VM_VMA_COW) &&
        !(vmm_pte_flags(*slot) & VM_PROT_WRITE)) {
        uint64_t pa = vmm_pte_paddr(*slot);
        uint32_t refs = cow_refs(pa);

        if (refs <= 1) {
            /* Sole owner: upgrade in place, no copy. */
            if (refs == 1) {
                spin_lock(&frame_refs_lock);
                frame_refs[pa >> PAGE_SHIFT] = 0;
                spin_unlock(&frame_refs_lock);
            }
            vmm_pte_write_leaf(slot, pa, v->flags);
        } else {
            /* Shared: private copy for the faulting space. */
            uint64_t npa = pmm_alloc();
            if (npa == 0) {
                spin_unlock(&as->lock);
                return false;           /* OOM: caller kills */
            }
            memcpy((void *)boot_phys_to_virt(npa),
                   (void *)boot_phys_to_virt(pa), PAGE_SIZE);
            vmm_pte_write_leaf(slot, npa, v->flags);
            cow_ref_dec(pa);            /* drops to 1: parent sole owner */
            vm_cow_copies++;
        }

        if (as == active_as)
            vmm_tlb_inval(va);
        spin_unlock(&as->lock);
        return true;
    }

    /* A resident page faulting again is a genuine permission violation,
     * not something demand paging can fix. */
    if (slot != NULL && level == 1 && vmm_pte_present(*slot)) {
        spin_unlock(&as->lock);
        return false;
    }

    /* Anonymous demand-zero fill. */
    uint64_t frame = vm_map_page(as, va, v->flags);

    spin_unlock(&as->lock);
    return frame != 0;      /* PMM empty: unrecoverable for the caller */
}

void vm_as_activate(vm_as_t *as)
{
    uint64_t target = as ? as->root_pa : vmm_kernel_root();

#if defined(__aarch64__)
    if (target == 0)
        target = a64_null_root;
#endif

    if (active_as == as && arch_cur_root() == target)
        return;

    arch_set_root(target);
    active_as = as;
}

void vm_init(void)
{
    frame_refs_init();

#if defined(__aarch64__)
    a64_null_root = pmm_alloc();
    if (a64_null_root == 0)
        panic("vm: no frame for the null user root");
    memset((void *)boot_phys_to_virt(a64_null_root), 0, PAGE_SIZE);
    arch_set_root(a64_null_root);
#endif

    active_as = NULL;

    kprintf("vm: process address spaces online (user half = "
            "0x0..0x%llx, %u-level walk)\n",
            (unsigned long long)(vmm_user_va_limit() - 1),
            vmm_user_levels());
}

vm_as_t *vm_as_create(void)
{
    vm_as_t *as = kmalloc(sizeof *as);
    if (as == NULL)
        return NULL;

    as->root_pa = pmm_alloc();
    if (as->root_pa == 0) {
        kfree(as);
        return NULL;
    }
    memset((void *)boot_phys_to_virt(as->root_pa), 0, PAGE_SIZE);

    as->table_pages = 1;
    as->resident_pages = 0;
    as->vmas = NULL;
    as->lock = (spinlock_t)SPINLOCK_INIT;

    /* Share the kernel higher half into this root. */
    vmm_copy_kernel_half(as->root_pa);

    return as;
}

void vm_as_destroy(vm_as_t *as)
{
    if (as == NULL)
        return;
    if (as == active_as)
        vm_as_activate(NULL);

    /* Release every resident page and the VMA list, then the tables.
     * No concurrent user of the space at destroy time. */
    struct vm_vma *v = as->vmas;
    while (v) {
        struct vm_vma *next = v->next;
        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE)
            vm_unmap_page(as, va);
        kfree(v);
        v = next;
    }

    vmm_free_user_tables(as->root_pa);

    kfree(as);
}

vm_as_t *vm_as_clone(vm_as_t *src)
{
    if (src == NULL)
        return NULL;
    frame_refs_init();

    vm_as_t *dst = vm_as_create();
    if (dst == NULL)
        return NULL;

    spin_lock(&src->lock);
    spin_lock(&dst->lock);

    /* Copy the VMA list. */
    struct vm_vma **tail = &dst->vmas;
    for (struct vm_vma *v = src->vmas; v; v = v->next) {
        struct vm_vma *nv = kmalloc(sizeof *nv);
        if (nv == NULL)
            goto oom;
        nv->start = v->start;
        nv->end = v->end;
        nv->flags = v->flags;
        nv->next = NULL;
        *tail = nv;
        tail = &nv->next;
    }

    /* Share resident pages: writable ones become COW in both spaces,
     * read-only ones are shared as-is. */
    struct vm_vma *dv = dst->vmas;
    for (struct vm_vma *v = src->vmas; v; v = v->next, dv = dv->next) {
        if (v->flags & VM_VMA_GUARD)
            continue;

        bool downgrade = (v->flags & VM_PROT_WRITE) != 0;
        if (downgrade) {
            dv->flags |= VM_VMA_COW;
            v->flags |= VM_VMA_COW;     /* parent keeps the flag too */
        }

        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
            unsigned level;
            uint64_t *sslot = vmm_pte_slot(src->root_pa, va, false,
                                           &level, NULL);
            if (sslot == NULL || level != 1 || !vmm_pte_present(*sslot))
                continue;

            uint64_t pa = vmm_pte_paddr(*sslot);
            unsigned sflags = vmm_pte_flags(*sslot);
            unsigned dflags = dv->flags;

            if (downgrade) {
                unsigned ro = dv->flags & ~(unsigned)VM_PROT_WRITE;
                if (sflags & VM_PROT_WRITE) {
                    /* Downgrade the parent in place. */
                    vmm_pte_write_leaf(sslot, pa, ro);
                    if (src == active_as)
                        vmm_tlb_inval(va);
                    cow_ref_inc(pa);
                } else if (cow_refs(pa) > 0) {
                    cow_ref_inc(pa);    /* already shared */
                }
                dflags = ro;
            }

            unsigned dcreated = 0;
            unsigned dlevel;
            uint64_t *dslot = vmm_pte_slot(dst->root_pa, va, true,
                                           &dlevel, &dcreated);
            dst->table_pages += dcreated;
            if (dslot == NULL || dlevel != 1)
                goto oom;
            vmm_pte_write_leaf(dslot, pa, dflags);
            dst->resident_pages++;
        }
    }

    spin_unlock(&dst->lock);
    spin_unlock(&src->lock);
    return dst;

oom:
    spin_unlock(&dst->lock);
    spin_unlock(&src->lock);
    /* The parent may have been downgraded; that is safe — pages with
     * refcount >= 1 upgrade in place again on the next write fault. */
    vm_as_destroy(dst);
    return NULL;
}
