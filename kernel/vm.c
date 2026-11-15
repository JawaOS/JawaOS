#include <stdint.h>
#include <stddef.h>

#include "boot.h"
#include "console.h"
#include "kheap.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"
#include "vm.h"
#include "vmm.h"

struct vm_as {
    uint64_t root_pa;
    uint64_t table_pages;
    spinlock_t lock;
};

/* This CPU's active user address space (NULL = kernel root). A per-CPU
 * variable once SMP bring-up lands; the TLB shootdown layer is built
 * around that model already. */
static vm_as_t *active_as;

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

    vmm_free_user_tables(as->root_pa);

    kfree(as);
}
