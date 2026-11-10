#include <stdint.h>
#include <stddef.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "pmm.h"
#include "spinlock.h"
#include "string.h"
#include "vmm.h"

extern const char __text_start[], __text_end[];
extern const char __rodata_start[], __rodata_end[];
extern const char __data_start[], __data_end[];
extern const char __bss_start[], __bss_end[];
extern const char __kernel_end[];

static uint64_t vmax(uint64_t a, uint64_t b) { return a > b ? a : b; }

#if defined(__x86_64__)

#define VMM_FIXED_LEVELS 4

#define X86_P     (1ull << 0)
#define X86_RW    (1ull << 1)
#define X86_US    (1ull << 2)
#define X86_PCD   (1ull << 4)

#define X86_A     (1ull << 5)
#define X86_D     (1ull << 6)
#define X86_PS    (1ull << 7)
#define X86_NX    (1ull << 63)

#define PTE_PHYS_MASK 0x000ffffffffff000ull

static unsigned arch_levels(void)
{
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ull << 12))
        panic("vmm: 5-level paging (LA57) active — only 4-level supported");
    return VMM_FIXED_LEVELS;
}

static uint64_t arch_root_alloc(void)
{
    return bootmem_alloc(1);
}

static uint64_t pte_present(uint64_t e) { return e & X86_P; }

static bool pte_is_leaf(uint64_t e, unsigned level)
{
    return (e & X86_P) != 0 && level == 2 && (e & X86_PS) != 0;
}

static uint64_t pte_table_paddr(uint64_t e) { return e & PTE_PHYS_MASK; }

static uint64_t pte_table(uint64_t pa)
{
    return X86_P | X86_RW | X86_A | (pa & PTE_PHYS_MASK);
}

static uint64_t pte_leaf(uint64_t pa, unsigned flags)
{
    uint64_t e = X86_P | X86_A | (pa & PTE_PHYS_MASK);

    if (flags & VM_PROT_WRITE)
        e |= X86_RW | X86_D;
    if (flags & VM_PROT_USER)
        e |= X86_US;
    if (flags & VM_PROT_DEVICE)
        e |= X86_PCD;
    if (!(flags & VM_PROT_EXEC))
        e |= X86_NX;
    return e;
}

static bool arch_huge_ok(unsigned level)
{
#if defined(__x86_64__)
    return level == 2;
#else
    return level == 2 || level == 3;
#endif
}

static uint64_t pte_huge(uint64_t pa, unsigned flags, unsigned level)
{
    if (!arch_huge_ok(level))
        panic("vmm: huge page at unsupported level %u", level);
    return pte_leaf(pa, flags) | X86_PS;
}

static unsigned pte_flags(uint64_t e)
{
    unsigned f = VM_PROT_READ;
    if (e & X86_RW) f |= VM_PROT_WRITE;
    if (!(e & X86_NX)) f |= VM_PROT_EXEC;
    if (e & X86_US) f |= VM_PROT_USER;
    if (e & X86_PCD) f |= VM_PROT_DEVICE;
    return f;
}

static void arch_install_root(uint64_t root_pa)
{

    __asm__ volatile ("mov %0, %%cr3" : : "r"(root_pa) : "memory");
}

static void arch_tlb_inval_page(uint64_t va)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(va) : "memory");
}

static const char *arch_paging_mode(void) { return "x86_64 4-level (CR3)"; }

#elif defined(__aarch64__)

#define VMM_FIXED_LEVELS 4

#define AA_TYPE_MASK  0x3ull
#define AA_TYPE_TABLE 0x3ull
#define AA_TYPE_BLOCK 0x1ull
#define AA_ATTRIDX(n) ((uint64_t)(n) << 2)
#define AA_AP_EL0     (1ull << 6)
#define AA_AP_RO      (1ull << 7)
#define AA_SH_IS      (2ull << 8)
#define AA_AF         (1ull << 10)

#define AA_PXN        (1ull << 53)
#define AA_UXN        (1ull << 54)

#define PTE_PHYS_MASK 0x0000fffffffff000ull

static unsigned arch_levels(void)
{

    uint64_t tcr;
    __asm__ volatile ("mrs %0, tcr_el1" : "=r"(tcr));

    if (((tcr >> 30) & 3ull) != 2ull)
        panic("vmm: TTBR1 granule is not 4 KiB (TCR_EL1.TG1=%u)",
              (unsigned)((tcr >> 30) & 3ull));
    if (((tcr >> 16) & 0x3Full) != 16ull)
        panic("vmm: T1SZ=%u, only 48-bit VA (T1SZ=16) supported",
              (unsigned)((tcr >> 16) & 0x3Full));
    return VMM_FIXED_LEVELS;
}

static uint64_t arch_root_alloc(void)
{

    return bootmem_alloc(4);
}

static uint64_t pte_present(uint64_t e) { return e & 0x1; }

static bool pte_is_leaf(uint64_t e, unsigned level)
{
    return (e & AA_TYPE_MASK) == AA_TYPE_BLOCK && level >= 2;
}

static uint64_t pte_table_paddr(uint64_t e) { return e & PTE_PHYS_MASK; }

static uint64_t pte_table(uint64_t pa)
{

    return AA_TYPE_TABLE | (pa & PTE_PHYS_MASK);
}

static uint64_t pte_leaf(uint64_t pa, unsigned flags)
{

    uint64_t e = AA_TYPE_TABLE | AA_AF | AA_SH_IS
               | AA_ATTRIDX((flags & VM_PROT_DEVICE) ? 1 : 0)
               | (pa & PTE_PHYS_MASK);

    if (!(flags & VM_PROT_WRITE))
        e |= AA_AP_RO;
    if (flags & VM_PROT_USER)
        e |= AA_AP_EL0;
    else
        e |= AA_UXN;
    if (!(flags & VM_PROT_EXEC))
        e |= AA_PXN;
    return e;
}

static bool arch_huge_ok(unsigned level)
{
    return level == 2 || level == 3;
}

static uint64_t pte_huge(uint64_t pa, unsigned flags, unsigned level)
{

    if (!arch_huge_ok(level))
        panic("vmm: aarch64 blocks are 2 MiB/1 GiB only (level %u)", level);
    return (pte_leaf(pa, flags) & ~AA_TYPE_MASK) | AA_TYPE_BLOCK;
}

static unsigned pte_flags(uint64_t e)
{
    unsigned f = VM_PROT_READ;
    if (!(e & AA_AP_RO)) f |= VM_PROT_WRITE;
    if (!(e & AA_PXN)) f |= VM_PROT_EXEC;
    if (e & AA_AP_EL0) f |= VM_PROT_USER;
    if (((e >> 2) & 0x7) == 1) f |= VM_PROT_DEVICE;
    return f;
}

static void arch_install_root(uint64_t root_pa)
{

    const uint64_t mair = (0x04ull << 8) | 0xFFull;

    __asm__ volatile (
        "msr mair_el1, %0\n"
        "dsb sy\n"
        "msr ttbr1_el1, %1\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb"
        : : "r"(mair), "r"(root_pa) : "memory");
}

static void arch_tlb_inval_page(uint64_t va)
{
    __asm__ volatile (
        "dsb ishst\n"
        "tlbi vaae1is, %0\n"
        "dsb sy\n"
        "isb"
        : : "r"(va >> 12) : "memory");
}

static const char *arch_paging_mode(void)
{
    return "aarch64 4 KiB granule, 4-level (TTBR1_EL1)";
}

#elif defined(__riscv)

#define RV_V (1ull << 0)
#define RV_R (1ull << 1)
#define RV_W (1ull << 2)
#define RV_X (1ull << 3)
#define RV_U (1ull << 4)
#define RV_A (1ull << 6)
#define RV_D (1ull << 7)

static unsigned vmm_levels;

static unsigned arch_levels(void)
{
    uint64_t satp;
    __asm__ volatile ("csrr %0, satp" : "=r"(satp));

    switch (satp >> 60) {
    case 8:  return 3;
    case 9:  return 4;
    case 10: return 5;
    default:
        panic("vmm: satp mode %u unsupported", (unsigned)(satp >> 60));
    }
}

static uint64_t arch_root_alloc(void)
{
    return bootmem_alloc(1);
}

static uint64_t pte_present(uint64_t e) { return e & RV_V; }

static bool pte_is_leaf(uint64_t e, unsigned level)
{

    (void)level;
    return (e & RV_V) != 0 && (e & (RV_R | RV_W | RV_X)) != 0;
}

static uint64_t pte_table_paddr(uint64_t e) { return (e >> 10) << 12; }

static uint64_t pte_table(uint64_t pa)
{
    return RV_V | ((pa >> PAGE_SHIFT) << 10);
}

static uint64_t pte_leaf(uint64_t pa, unsigned flags)
{

    uint64_t e = RV_V | RV_R | RV_A | RV_D | ((pa >> PAGE_SHIFT) << 10);

    if (flags & VM_PROT_WRITE) e |= RV_W;
    if (flags & VM_PROT_EXEC)  e |= RV_X;
    if (flags & VM_PROT_USER)  e |= RV_U;
    return e;
}

static bool arch_huge_ok(unsigned level)
{
    return level == 2 || level == 3;
}

static uint64_t pte_huge(uint64_t pa, unsigned flags, unsigned level)
{

    if (!arch_huge_ok(level))
        panic("vmm: riscv64 superpages are 2 MiB/1 GiB only (level %u)",
              level);
    return pte_leaf(pa, flags);
}

static unsigned pte_flags(uint64_t e)
{
    unsigned f = 0;
    if (e & RV_R) f |= VM_PROT_READ;
    if (e & RV_W) f |= VM_PROT_WRITE;
    if (e & RV_X) f |= VM_PROT_EXEC;
    if (e & RV_U) f |= VM_PROT_USER;
    return f;
}

static void arch_install_root(uint64_t root_pa)
{
    uint64_t satp, mode, new_satp;

    __asm__ volatile ("csrr %0, satp" : "=r"(satp));
    mode = satp >> 60;
    new_satp = (mode << 60) | (root_pa >> PAGE_SHIFT);

    __asm__ volatile (
        "fence rw, rw\n"
        "csrw satp, %0\n"
        "sfence.vma zero, zero"
        : : "r"(new_satp) : "memory");
}

static void arch_tlb_inval_page(uint64_t va)
{
    __asm__ volatile ("sfence.vma %0, zero" : : "r"(va) : "memory");
}

static const char *arch_paging_mode(void)
{
    switch (vmm_levels) {
    case 3:  return "riscv64 SV39 (satp)";
    case 4:  return "riscv64 SV48 (satp)";
    case 5:  return "riscv64 SV57 (satp)";
    default: return "riscv64 (?)";
    }
}

#else
#error "unsupported architecture for the VMM"
#endif

#ifdef VMM_FIXED_LEVELS
static unsigned levels(void) { return VMM_FIXED_LEVELS; }
#else
static unsigned levels(void) { return vmm_levels; }
#endif

static uint64_t *vmm_root;
static uint64_t vmm_root_pa;
static uint64_t hhdm_limit;
static uint64_t table_pages;
static spinlock_t vmm_lock = SPINLOCK_INIT;

static inline unsigned pt_index(uint64_t va, unsigned level)
{
    return (unsigned)((va >> (PAGE_SHIFT + 9 * (level - 1))) & 0x1FF);
}

static inline uint64_t level_leaf_size(unsigned level)
{
    return 1ull << (PAGE_SHIFT + 9 * (level - 1));
}

static uint64_t *pt_walk(uint64_t va, unsigned target_level, bool create,
                         unsigned *out_level)
{
    uint64_t *table = vmm_root;

    for (unsigned level = levels(); ; level--) {
        uint64_t *slot = &table[pt_index(va, level)];

        if (level == target_level) {
            *out_level = level;
            return slot;
        }

        uint64_t e = *slot;
        if (!pte_present(e)) {
            if (!create) {
                *out_level = level;
                return slot;
            }
            uint64_t newtab = pmm_alloc();
            if (newtab == 0)
                panic("vmm: PMM exhausted while building page tables");
            memset((void *)boot_phys_to_virt(newtab), 0, PAGE_SIZE);
            table_pages++;
            *slot = pte_table(newtab);
            e = *slot;
        } else if (pte_is_leaf(e, level)) {
            *out_level = level;
            return slot;
        }

        table = (uint64_t *)boot_phys_to_virt(pte_table_paddr(e));
    }
}

static void map_range(uint64_t vaddr, uint64_t paddr, uint64_t size,
                      unsigned flags)
{
    while (size > 0) {
        bool placed = false;

        for (unsigned level = levels(); level >= 2 && !placed; level--) {
            if (!arch_huge_ok(level))
                continue;

            uint64_t leaf = level_leaf_size(level);

            if (size < leaf || (vaddr & (leaf - 1)) != 0 ||
                (paddr & (leaf - 1)) != 0)
                continue;

            unsigned slot_level;
            uint64_t *slot = pt_walk(vaddr, level, true, &slot_level);
            if (slot_level != level)
                panic("vmm: map_range blocked at level %u (want %u)",
                      slot_level, level);
            *slot = pte_huge(paddr, flags, level);
            vaddr += leaf;
            paddr += leaf;
            size -= leaf;
            placed = true;
        }

        if (!placed) {
            unsigned slot_level;
            uint64_t *slot = pt_walk(vaddr, 1, true, &slot_level);
            if (slot_level != 1)
                panic("vmm: map_range blocked by a huge page at 0x%llx",
                      (unsigned long long)vaddr);
            *slot = pte_leaf(paddr, flags);
            vaddr += PAGE_SIZE;
            paddr += PAGE_SIZE;
            size -= PAGE_SIZE;
        }
    }
}

void vm_map(uint64_t vaddr, uint64_t paddr, unsigned flags)
{
    if (!page_aligned(vaddr) || !page_aligned(paddr))
        panic("vmm: vm_map of misaligned addresses (0x%llx <- 0x%llx)",
              (unsigned long long)vaddr, (unsigned long long)paddr);
    if ((flags & (VM_PROT_WRITE | VM_PROT_EXEC)) ==
        (VM_PROT_WRITE | VM_PROT_EXEC))
        panic("vmm: W^X violation — refusing to map 0x%llx write+execute",
              (unsigned long long)vaddr);
    if ((flags & (VM_PROT_DEVICE | VM_PROT_EXEC)) ==
        (VM_PROT_DEVICE | VM_PROT_EXEC))
        panic("vmm: refusing to map 0x%llx device+execute",
              (unsigned long long)vaddr);
    if (!(flags & VM_PROT_READ))
        panic("vmm: vm_map without VM_PROT_READ (0x%llx)",
              (unsigned long long)vaddr);

    spin_lock(&vmm_lock);

    unsigned level;
    uint64_t *slot = pt_walk(vaddr, 1, true, &level);
    if (level != 1)
        panic("vmm: vm_map over a huge mapping at 0x%llx",
              (unsigned long long)vaddr);

    bool was_mapped = pte_present(*slot) != 0;
    *slot = pte_leaf(paddr, flags);

    spin_unlock(&vmm_lock);

    if (was_mapped)
        arch_tlb_inval_page(vaddr);
}

bool vm_unmap(uint64_t vaddr)
{
    if (!page_aligned(vaddr))
        panic("vmm: vm_unmap of misaligned address 0x%llx",
              (unsigned long long)vaddr);

    spin_lock(&vmm_lock);

    unsigned level;
    uint64_t *slot = pt_walk(vaddr, 1, false, &level);

    bool was_mapped = slot != NULL && pte_present(*slot) != 0;
    if (was_mapped) {
        *slot = 0;

    }

    spin_unlock(&vmm_lock);

    if (was_mapped)
        arch_tlb_inval_page(vaddr);
    return was_mapped;
}

bool vmm_query(uint64_t vaddr, uint64_t *paddr, unsigned *flags)
{
    uint64_t page = page_align_down(vaddr);

    unsigned level;
    uint64_t *slot = pt_walk(page, 1, false, &level);
    if (slot == NULL || !pte_present(*slot))
        return false;

    uint64_t base = pte_table_paddr(*slot);
    uint64_t phys = base + (page & (level_leaf_size(level) - 1));

    if (paddr)
        *paddr = phys;
    if (flags)
        *flags = pte_flags(*slot);
    return true;
}

uint64_t vmm_root_phys(void)    { return vmm_root_pa; }
uint64_t vmm_hhdm_limit(void)   { return hhdm_limit; }
uint64_t vmm_table_pages(void)  { return table_pages; }

const char *vmm_paging_mode_name(void) { return arch_paging_mode(); }

static uint64_t device_next = VMM_DEVICE_BASE;
static spinlock_t device_lock = SPINLOCK_INIT;

void *vmm_map_device(uint64_t phys, uint64_t size)
{
    if (size == 0)
        return NULL;
    if (!page_aligned(phys))
        panic("vmm: vmm_map_device of unaligned physical 0x%llx",
              (unsigned long long)phys);
    if (vmm_root == NULL)
        panic("vmm: vmm_map_device before vmm_init()");

    uint64_t npages = page_align_up(size) >> PAGE_SHIFT;

    spin_lock(&device_lock);
    if (device_next + npages * PAGE_SIZE >
        VMM_DEVICE_BASE + VMM_DEVICE_SIZE)
        panic("vmm: device window exhausted");
    uint64_t va = device_next;
    device_next += npages * PAGE_SIZE;
    spin_unlock(&device_lock);

    for (uint64_t i = 0; i < npages; i++)
        vm_map(va + i * PAGE_SIZE, phys + i * PAGE_SIZE,
               VM_PROT_READ | VM_PROT_WRITE | VM_PROT_DEVICE);

    return (void *)va;
}

static bool ranges_overlap(uint64_t a1, uint64_t a2, uint64_t b1, uint64_t b2)
{
    return a1 < b2 && b1 < a2;
}

void vmm_init(void)
{
    table_pages = 0;

#ifdef VMM_FIXED_LEVELS
    arch_levels();
#else
    vmm_levels = arch_levels();
#endif

    vmm_root_pa = arch_root_alloc();
    table_pages++;
    vmm_root = (uint64_t *)boot_phys_to_virt(vmm_root_pa);
    memset(vmm_root, 0, PAGE_SIZE);

    const uint64_t slide =
        bootinfo.kernel_virt_base - bootinfo.kernel_phys_base;

    map_range((uint64_t)__text_start, (uint64_t)__text_start - slide,
              (uint64_t)__rodata_start - (uint64_t)__text_start,
              VM_PROT_READ | VM_PROT_EXEC);

    map_range((uint64_t)__rodata_start, (uint64_t)__rodata_start - slide,
              (uint64_t)__data_start - (uint64_t)__rodata_start,
              VM_PROT_READ);

    map_range((uint64_t)__data_start, (uint64_t)__data_start - slide,
              (uint64_t)__kernel_end - (uint64_t)__data_start,
              VM_PROT_READ | VM_PROT_WRITE);

    uint64_t limit = 0;
    for (uint64_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct boot_memmap_entry *e = &bootinfo.memmap[i];
        if (e->type == LIMINE_MEMMAP_RESERVED ||
            e->type == LIMINE_MEMMAP_BAD_MEMORY)
            continue;
        limit = vmax(limit, page_align_up(e->base + e->length));
    }
    if (bootinfo.fb.phys_end != 0)
        limit = vmax(limit, bootinfo.fb.phys_end);

    if (ranges_overlap(bootinfo.hhdm_offset, bootinfo.hhdm_offset + limit,
                       (uint64_t)__text_start, (uint64_t)__kernel_end) ||
        ranges_overlap(bootinfo.hhdm_offset, bootinfo.hhdm_offset + limit,
                       KHEAP_BASE, KHEAP_BASE + KHEAP_MAX_SIZE) ||
        ranges_overlap(bootinfo.hhdm_offset, bootinfo.hhdm_offset + limit,
                       VMM_SCRATCH_BASE, VMM_SCRATCH_BASE + VMM_SCRATCH_SIZE) ||
        ranges_overlap(bootinfo.hhdm_offset, bootinfo.hhdm_offset + limit,
                       VMM_DEVICE_BASE, VMM_DEVICE_BASE + VMM_DEVICE_SIZE))
        panic("vmm: HHDM range overlaps the kernel image/heap/scratch/device window");

    map_range(bootinfo.hhdm_offset, 0, limit,
              VM_PROT_READ | VM_PROT_WRITE);

    arch_install_root(vmm_root_pa);

    uint64_t pa;
    unsigned fl;

    if (!vmm_query((uint64_t)__text_start, &pa, &fl) ||
        pa != (uint64_t)__text_start - slide ||
        (fl & (VM_PROT_EXEC | VM_PROT_WRITE)) != VM_PROT_EXEC)
        panic("vmm: kernel text is not RX after takeover");

    if (!vmm_query((uint64_t)__data_start, &pa, &fl) ||
        (fl & (VM_PROT_READ | VM_PROT_WRITE)) !=
            (VM_PROT_READ | VM_PROT_WRITE) ||
        (fl & VM_PROT_EXEC) != 0)
        panic("vmm: kernel data is not RW after takeover");

    if (!vmm_query(bootinfo.hhdm_offset + vmm_root_pa, &pa, &fl) ||
        pa != vmm_root_pa || (fl & VM_PROT_WRITE) == 0)
        panic("vmm: HHDM does not cover the root table");

    uint64_t test = pmm_alloc();
    if (test == 0)
        panic("vmm: PMM empty during takeover self-test");
    volatile uint64_t *p = (volatile uint64_t *)boot_phys_to_virt(test);
    *p = 0x1234567890abcdefull;
    if (*p != 0x1234567890abcdefull)
        panic("vmm: HHDM canary write/read failed");
    pmm_free(test);

    volatile uint64_t local = 0;
    if (!vmm_query((uint64_t)&local, NULL, NULL))
        panic("vmm: kernel stack unmapped after takeover");

    hhdm_limit = limit;
}
