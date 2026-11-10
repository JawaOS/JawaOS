#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "int.h"
#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "vmm.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER

static inline void cpu_idle(void) {
#if defined(__x86_64__)
    __asm__ volatile ("hlt");
#elif defined(__aarch64__)
    __asm__ volatile ("wfi");
#elif defined(__riscv)
    __asm__ volatile ("wfi");
#elif defined(__loongarch__)
    __asm__ volatile ("idle 0");
#endif
}

static const char *arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__riscv)
    return "riscv64";
#else
    return "unknown";
#endif
}

static void boot_report(void)
{
    extern const char __text_start[], __kernel_end[];

    kprintf("\n--- boot data (Limine, base revision %llu) ---\n",
            (unsigned long long)LIMINE_LOADED_BASE_REVISION);

    kprintf("memory map: %llu entries\n",
            (unsigned long long)bootinfo.memmap_count);
    for (uint64_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct boot_memmap_entry *e = &bootinfo.memmap[i];
        kprintf("  [%2llu] 0x%09llx-0x%09llx %8llu KiB  %s\n",
                (unsigned long long)i,
                (unsigned long long)e->base,
                (unsigned long long)(e->base + e->length - 1),
                (unsigned long long)(e->length >> 10),
                boot_memmap_type_name(e->type));
    }

    kprintf("HHDM offset:     0x%llx (span %llu MiB, NX, RW)\n",
            (unsigned long long)bootinfo.hhdm_offset,
            (unsigned long long)(vmm_hhdm_limit() >> 20));

    kprintf("kernel image:    phys 0x%llx, virt 0x%llx, %u KiB, W^X\n",
            (unsigned long long)bootinfo.kernel_phys_base,
            (unsigned long long)bootinfo.kernel_virt_base,
            (unsigned)((uint64_t)__kernel_end - (uint64_t)__text_start) / 1024);

#if defined(__x86_64__)
    if (bootinfo.rsdp != NULL)
        kprintf("ACPI RSDP:       0x%llx (HHDM)\n",
                (unsigned long long)(uint64_t)bootinfo.rsdp);
    else
        kprintf("ACPI RSDP:       not provided\n");
#else
    if (bootinfo.dtb != NULL)
        kprintf("device tree:     0x%llx (HHDM)\n",
                (unsigned long long)(uint64_t)bootinfo.dtb);
    else
        kprintf("device tree:     not provided\n");
#endif

    if (bootinfo.fb_present) {
        kprintf("framebuffer:     %llux%llu, %u bpp, pitch %llu\n",
                (unsigned long long)bootinfo.fb.width,
                (unsigned long long)bootinfo.fb.height,
                (unsigned)bootinfo.fb.bpp,
                (unsigned long long)bootinfo.fb.pitch);
    }

    kprintf("--- memory managers ---\n");
    kprintf("pmm:  %llu usable pages (%llu MiB), %llu free\n",
            (unsigned long long)pmm_total_usable_pages(),
            (unsigned long long)(pmm_total_usable_pages() >> 8),
            (unsigned long long)pmm_free_pages());
    kprintf("vmm:  %s, root @ phys 0x%llx, %llu table pages\n",
            vmm_paging_mode_name(),
            (unsigned long long)vmm_root_phys(),
            (unsigned long long)vmm_table_pages());
}

#define CHECK(cond, what) do {                                  \
        if (!(cond)) {                                          \
            panic("selftest: FAILED: %s", what);                \
        }                                                       \
        kprintf("  ok: %s\n", what);                            \
    } while (0)

static void selftest_pmm(void)
{
    kprintf("selftest: physical memory manager\n");

    uint64_t before = pmm_free_pages();
    uint64_t p[4];

    for (int i = 0; i < 4; i++) {
        p[i] = pmm_alloc();
        CHECK(p[i] != 0 && page_aligned(p[i]),
              "pmm_alloc returns aligned, nonzero frames");
    }
    CHECK(pmm_free_pages() == before - 4, "free count drops by 4");

    bool distinct = p[0] != p[1] && p[0] != p[2] && p[0] != p[3] &&
                    p[1] != p[2] && p[1] != p[3] && p[2] != p[3];
    CHECK(distinct, "allocated frames are distinct");

    for (int i = 0; i < 4; i++)
        *(volatile uint64_t *)boot_phys_to_virt(p[i]) = 0xC0FFEE00ull + (uint64_t)i;
    for (int i = 0; i < 4; i++)
        CHECK(*(volatile uint64_t *)boot_phys_to_virt(p[i]) ==
                  0xC0FFEE00ull + (uint64_t)i,
              "HHDM write/read roundtrip");

    for (int i = 0; i < 4; i++)
        pmm_free(p[i]);
    CHECK(pmm_free_pages() == before, "free count restored after pmm_free");

    uint64_t q[4];
    for (int i = 0; i < 4; i++)
        q[i] = pmm_alloc();
    CHECK(q[0] == p[3] && q[1] == p[2] && q[2] == p[1] && q[3] == p[0],
          "free list is LIFO (xv6-style stack)");
    for (int i = 0; i < 4; i++)
        pmm_free(q[i]);
    CHECK(pmm_free_pages() == before, "alloc/free cycle conserves free count");

    uint64_t r = pmm_alloc_range(4);
    CHECK(r != 0 && page_aligned(r), "pmm_alloc_range returns a base frame");
    CHECK(pmm_free_pages() == before - 4, "pmm_alloc_range: free count drops by 4");
    for (uint64_t i = 0; i < 4; i++)
        *(volatile uint64_t *)boot_phys_to_virt(r + i * PAGE_SIZE) =
            0x51515151ull + i;
    for (uint64_t i = 0; i < 4; i++)
        CHECK(*(volatile uint64_t *)boot_phys_to_virt(r + i * PAGE_SIZE) ==
                  0x51515151ull + i,
              "pmm_alloc_range: write/read across the contiguous run");
    for (uint64_t i = 0; i < 4; i++)
        pmm_free(r + i * PAGE_SIZE);
    CHECK(pmm_free_pages() == before, "pmm_alloc_range pages restored");
}

static void selftest_vmm(void)
{
    kprintf("selftest: virtual memory manager\n");

    extern const char __rodata_start[];

    uint64_t pa;
    unsigned fl;
    const uint64_t slide =
        bootinfo.kernel_virt_base - bootinfo.kernel_phys_base;

    CHECK(vmm_query((uint64_t)&selftest_vmm, &pa, &fl) &&
          (fl & VM_PROT_EXEC) && !(fl & VM_PROT_WRITE) &&
          pa == page_align_down((uint64_t)&selftest_vmm - slide),
          "kernel text is R+X (no write)");

    CHECK(vmm_query((uint64_t)__rodata_start, NULL, &fl) &&
          !(fl & VM_PROT_WRITE) && !(fl & VM_PROT_EXEC),
          "kernel rodata is R only (no write, no exec)");

    CHECK(vmm_query((uint64_t)&bootinfo, &pa, &fl) &&
          (fl & VM_PROT_WRITE) && !(fl & VM_PROT_EXEC),
          "kernel data is RW, no exec");

    CHECK(vmm_query(bootinfo.hhdm_offset + 0x1000, &pa, &fl) &&
          pa == 0x1000 && (fl & VM_PROT_WRITE) && !(fl & VM_PROT_EXEC),
          "HHDM maps phys 0x1000 RW, no exec");

    CHECK(!vmm_query(VMM_SCRATCH_BASE, NULL, NULL),
          "scratch region starts unmapped");

    uint64_t f1 = pmm_alloc();
    uint64_t f2 = pmm_alloc();
    volatile uint64_t *va = (volatile uint64_t *)VMM_SCRATCH_BASE;

    vm_map(VMM_SCRATCH_BASE, f1, VM_PROT_READ | VM_PROT_WRITE);
    *va = 0x1111ull;
    CHECK(*va == 0x1111ull && vmm_query(VMM_SCRATCH_BASE, &pa, &fl) &&
          pa == f1 && (fl & VM_PROT_WRITE) && !(fl & VM_PROT_EXEC),
          "vm_map: fresh 4K page works (RW, NX)");

    vm_map(VMM_SCRATCH_BASE, f2, VM_PROT_READ | VM_PROT_WRITE);
    *va = 0x2222ull;
    CHECK(*va == 0x2222ull && vmm_query(VMM_SCRATCH_BASE, &pa, &fl) &&
          pa == f2,
          "vm_map over an existing mapping sees the new frame");

    CHECK(vm_unmap(VMM_SCRATCH_BASE) && !vmm_query(VMM_SCRATCH_BASE, NULL, NULL),
          "vm_unmap removes the mapping");
    CHECK(vm_unmap(VMM_SCRATCH_BASE) == false,
          "vm_unmap of an unmapped page returns false");

    uint64_t df = pmm_alloc();
    volatile uint64_t *dv = vmm_map_device(df, PAGE_SIZE);
    CHECK(dv != NULL && (uint64_t)dv >= VMM_DEVICE_BASE &&
          (uint64_t)dv < VMM_DEVICE_BASE + VMM_DEVICE_SIZE,
          "vmm_map_device: VA inside the device window");
    *dv = 0xDE1EFEEDull;
    CHECK(*dv == 0xDE1EFEEDull, "vmm_map_device: write/read via device alias");
    unsigned dfl = 0;
    CHECK(vmm_query((uint64_t)dv, NULL, &dfl) &&
          (dfl & VM_PROT_WRITE) && !(dfl & VM_PROT_EXEC),
          "vmm_query sees the device mapping");
    pmm_free(df);

    pmm_free(f1);
    pmm_free(f2);
}

static void selftest_kheap(void)
{
    kprintf("selftest: kernel heap\n");

    uint64_t a0, m0, a1, m1, m2;
    kheap_stats(&a0, &m0);

    CHECK(kmalloc(0) == NULL, "kmalloc(0) returns NULL");

    void *p[6] = {0};
    size_t sz[6] = { 16, 100, 4096, 100000, 3, 0x10000 };
    for (int i = 0; i < 6; i++) {
        p[i] = kmalloc(sz[i]);
        CHECK(p[i] != NULL && ((uint64_t)p[i] & 0xF) == 0,
              "kmalloc returns 16-byte aligned memory");
    }

    for (int i = 0; i < 6; i++)
        memset(p[i], 0x5A, sz[i]);
    for (int i = 0; i < 6; i++) {
        volatile uint8_t *b = p[i];
        CHECK(b[0] == 0x5A && b[sz[i] - 1] == 0x5A,
              "pattern write/read across allocated blocks");
    }

#ifdef JAWA_DEBUG

    unsigned char *q = kmalloc(64);
    CHECK(q[0] == KHEAP_POISON_ALLOC && q[63] == KHEAP_POISON_ALLOC,
          "debug: fresh kmalloc is poisoned 0xAA");
    kfree(q);
    CHECK(q[16] == KHEAP_POISON_FREE,
          "debug: freed block is poisoned 0xDE");
#endif

    for (int i = 0; i < 6; i++)
        kfree(p[i]);
    kheap_stats(&a1, &m1);
    CHECK(a1 == a0, "all-freed heap reports 0 live bytes");

    void *x = kmalloc(64);
    void *y = kmalloc(64);
    void *z = kmalloc(64);
    CHECK(x && y && z &&
          (uint64_t)y == (uint64_t)x + 80 && (uint64_t)z == (uint64_t)y + 80,
          "first-fit packs consecutive allocations adjacently");

    kfree(x);
    kfree(z);
    kfree(y);

    kheap_stats(&a1, &m2);
    void *big = kmalloc(160);
    kheap_stats(&a1, &m1);
    CHECK(big != NULL && m1 == m2,
          "out-of-order frees coalesce (no heap growth on reuse)");
    kfree(big);

    uint64_t af, mf;
    kheap_stats(&af, &mf);
    CHECK(af == a0, "allocated bytes back to baseline after freeing all");
    kprintf("  ok: heap stats: %llu bytes live, %llu KiB mapped\n",
            (unsigned long long)af, (unsigned long long)(mf >> 10));
}

#define BOOT_STACK_PAGES 8

static void kmain_cont(void);

__attribute__((naked))
static void stack_switch(uint64_t sp __attribute__((unused)),
                         void (*fn)(void) __attribute__((unused)))
{
#if defined(__x86_64__)

    __asm__ volatile ("mov %rdi, %rsp\n\tjmp *%rsi");
#elif defined(__aarch64__)
    /* Limine enters at EL1t (SP_EL0) with SP_EL1 still holding its stale
     * physical stack. SPSel=1 selects SP_EL1 as the current stack, which
     * also flips PSTATE to EL1h; exception entry then lands on this stack. */
    __asm__ volatile ("msr spsel, #1\n\tmov sp, x0\n\tbr x1");
#elif defined(__riscv)
    __asm__ volatile ("mv sp, a0\n\tjr a1");
#else
#error "unsupported architecture for stack_switch"
#endif
}

static void boot_stack_switch(void)
{
    uint64_t phys = pmm_alloc_range(BOOT_STACK_PAGES);
    if (phys == 0)
        panic("boot: no contiguous run for the kernel stack");

    uint64_t top = boot_phys_to_virt(phys) + BOOT_STACK_PAGES * PAGE_SIZE;
#if defined(__x86_64__)
    top -= 8;
#endif
    stack_switch(top, kmain_cont);
}

void kmain(void) {
#if defined(__x86_64__)

    serial_init();
#endif

    if (!LIMINE_BASE_REVISION_SUPPORTED)
        panic("boot: bootloader does not support Limine base revision 6");

    int_init();

    boot_init();
    pmm_init();

    boot_stack_switch();
}

static void selftest_int(void)
{
    kprintf("selftest: exceptions and traps\n");

    CHECK(fault_probe(0x0) == false,
          "NULL dereference caught by trap handler, kernel survived");

    uint64_t canary = 0xC0DE5EEDull;
    CHECK(fault_probe((uint64_t)&canary) == true,
          "probe read of a mapped address succeeds");

    uint64_t bad = (uint64_t)0xffffdeadull << 32;
    CHECK(fault_probe(bad) == false,
          "unmapped canonical address caught, kernel survived");

    CHECK(int_breakpoint_selftest(),
          "breakpoint exception delivered to handler and resumed");
}

static void kmain_cont(void)
{
    vmm_init();
    console_init();
    kheap_init();

    kprintf("\nJawaOS v0.3.0 (rungs 2-5: memory, exceptions)\n");
    kprintf("  arch:        %s\n", arch_name());
    kprintf("  kprintf:     dec=%d u=%u hex=0x%08x ptr=%p str=%s ch=%c\n",
            -1234, 3000000000u, 0xcafebabe, (void *)&kmain, "ok", '!');

    boot_report();
    selftest_pmm();
    selftest_vmm();
    selftest_kheap();
    selftest_int();

    kprintf("\nmemory management online — idling.\n");

    for (;;) {
        cpu_idle();
    }
}
