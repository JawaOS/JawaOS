#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "int.h"
#include "kheap.h"
#include "spinlock.h"
#include "thread.h"
#include "timer.h"
#include "pmm.h"
#include "string.h"
#include "vm.h"
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

static uint64_t counters[3];

static void worker(uint64_t id)
{
    kprintf("  thread %llu running\n", (unsigned long long)id);
    for (int i = 0; i < 5; i++) {
        counters[id]++;
        yield();
    }
    kprintf("  thread %llu done\n", (unsigned long long)id);
}

static void selftest_thread(void)
{
    kprintf("selftest: threads and scheduler\n");

    for (uint64_t i = 0; i < 3; i++)
        kthread_create(worker, i);

    while (counters[0] + counters[1] + counters[2] < 15)
        yield();

    CHECK(counters[0] == 5 && counters[1] == 5 && counters[2] == 5,
          "three worker threads each ran to completion");
}

static void selftest_timer(void)
{
    kprintf("selftest: timers\n");
    kprintf("  ticks at entry: %llu\n", (unsigned long long)ticks_ms());

    uint64_t t0 = ticks_ms();
    msleep(150);
    uint64_t dt = ticks_ms() - t0;
    CHECK(dt >= 150, "msleep(150) sleeps at least 150 ms");
    CHECK(dt < 5000, "msleep(150) does not oversleep");
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

static void selftest_vm_as(void)
{
    kprintf("selftest: per-process address spaces\n");

    CHECK(vmm_user_va_limit() > PAGE_SIZE && vmm_user_levels() >= 2,
          "arch layer exposes a user VA half with a multi-level walk");

    vm_as_t *a = vm_as_create();
    vm_as_t *b = vm_as_create();
    CHECK(a != NULL && b != NULL, "vm_as_create succeeds");
    CHECK(vm_as_root(a) != 0 && vm_as_root(b) != 0 &&
          vm_as_root(a) != vm_as_root(b) &&
          vm_as_root(a) != vmm_kernel_root(),
          "each address space has a distinct root");

    /* Run as process A: the shared higher half must keep working and
     * the user half must be completely empty. */
    current->as = a;
    vm_as_activate(a);

    volatile uint64_t canary = 0xA5A55A5Aull;
    CHECK(canary == 0xA5A55A5Aull,
          "kernel half executes normally under a user root");
    CHECK(fault_probe(0x1000) == false,
          "fresh address space has an empty user half (faults)");
    CHECK(fault_probe((uint64_t)&canary) == true,
          "kernel half stays mapped under a user root");

    /* Switch to process B and back, the way the scheduler does. */
    current->as = b;
    vm_as_activate(b);
    CHECK(canary == 0xA5A55A5Aull,
          "second address space keeps the kernel half working");

    current->as = NULL;
    vm_as_activate(NULL);
    CHECK(vm_as_current() == NULL,
          "activate(NULL) restores the kernel root");

    /* Heap growth from the two kmallocs is permanent (the kheap never
     * returns pages), so measure after creation: destroying the two
     * empty spaces must hand back exactly their two root pages. */
    uint64_t before_destroy = pmm_free_pages();
    vm_as_destroy(a);
    vm_as_destroy(b);
    CHECK(pmm_free_pages() == before_destroy + 2,
          "destroying empty address spaces returns their root pages");
}

static void selftest_vm_vma(void)
{
    kprintf("selftest: memory-mapped regions\n");

    vm_as_t *as = vm_as_create();
    CHECK(as != NULL, "address space for VMA tests");

    /* Eager anonymous mapping. */
    uint64_t va = vm_mmap(as, 0, 3 * PAGE_SIZE,
                          VM_PROT_READ | VM_PROT_WRITE,
                          VM_VMA_ANON | VM_MAP_POPULATE);
    CHECK(va != 0 && page_aligned(va) && va >= VM_USER_BASE,
          "anonymous mmap lands page-aligned above VM_USER_BASE");

    unsigned level;
    uint64_t *slot = vmm_pte_slot(vm_as_root(as), va, false, &level, NULL);
    CHECK(slot != NULL && level == 1 && vmm_pte_present(*slot) &&
          (vmm_pte_flags(*slot) & VM_PROT_WRITE),
          "POPULATE maps the first page eagerly, read-write");
    slot = vmm_pte_slot(vm_as_root(as), va + 2 * PAGE_SIZE, false, &level, NULL);
    CHECK(slot != NULL && level == 1 && vmm_pte_present(*slot),
          "POPULATE maps the whole range");
    CHECK(vm_as_resident_pages(as) == 3,
          "resident count is 3 after POPULATE");

    /* Use it while it is the active address space. */
    current->as = as;
    vm_as_activate(as);
    volatile uint64_t *p = (volatile uint64_t *)va;
    p[0] = 0x1234;
    p[(3 * PAGE_SIZE) / 8 - 1] = 0x5678;
    CHECK(p[0] == 0x1234 && p[(3 * PAGE_SIZE) / 8 - 1] == 0x5678,
          "read/write through the active address space");
    current->as = NULL;
    vm_as_activate(NULL);

    /* Placement rules. */
    CHECK(vm_mmap(as, va, PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE,
                  VM_VMA_ANON | VM_MAP_FIXED) == 0,
          "VM_MAP_FIXED over an existing region rejected");
    CHECK(vm_mmap(as, 0, 0, VM_PROT_READ, VM_VMA_ANON) == 0,
          "zero-length mmap rejected");
    CHECK(vm_mmap(as, 0, PAGE_SIZE, VM_PROT_WRITE, VM_VMA_ANON) == 0,
          "mmap without VM_PROT_READ rejected");

    uint64_t va2 = vm_mmap(as, va + PAGE_SIZE, 2 * PAGE_SIZE,
                           VM_PROT_READ | VM_PROT_WRITE, VM_VMA_ANON);
    CHECK(va2 == va + 3 * PAGE_SIZE,
          "colliding preferred hint moves to the next free gap");

    /* Teardown. */
    CHECK(vm_munmap(as, va + PAGE_SIZE, PAGE_SIZE),
          "munmap of the middle page");
    CHECK(vm_as_resident_pages(as) == 2,
          "resident count drops to 2");
    slot = vmm_pte_slot(vm_as_root(as), va + PAGE_SIZE, false, &level, NULL);
    CHECK(slot == NULL || level != 1 || !vmm_pte_present(*slot),
          "middle page PTE cleared by munmap");

    uint64_t before = pmm_free_pages();
    CHECK(vm_munmap(as, va, 2 * PAGE_SIZE),
          "munmap of first page across a hole");
    CHECK(pmm_free_pages() == before + 1,
          "munmap returns exactly the one resident frame");

    CHECK(vm_munmap(as, va2, 2 * PAGE_SIZE),
          "munmap of the tail region (no resident pages there)");
    CHECK(vm_as_resident_pages(as) == 1,
          "one page remains resident in the kept remainder");
    CHECK(vm_munmap(as, va + 2 * PAGE_SIZE, PAGE_SIZE),
          "munmap of the kept remainder");
    CHECK(vm_as_resident_pages(as) == 0, "all resident pages released");
    CHECK(vm_munmap(as, va + 5 * PAGE_SIZE, PAGE_SIZE) == false,
          "munmap of an untouched range reports nothing removed");

    uint64_t vaf = vm_mmap(as, va, PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE,
                           VM_VMA_ANON | VM_MAP_FIXED);
    CHECK(vaf == va, "VM_MAP_FIXED at a free address is honored");
    CHECK(vm_munmap(as, va, PAGE_SIZE), "final munmap");

    vm_as_destroy(as);
}

static void selftest_vm_demand(void)
{
    kprintf("selftest: demand paging and fault recovery\n");

    vm_as_t *as = vm_as_create();
    CHECK(as != NULL, "address space for demand paging");

    /* Lazy by default: registered, but nothing resident. */
    uint64_t va = vm_mmap(as, 0, 4 * PAGE_SIZE,
                          VM_PROT_READ | VM_PROT_WRITE, VM_VMA_ANON);
    CHECK(va != 0 && vm_as_resident_pages(as) == 0,
          "lazy mmap is registered but maps nothing");

    unsigned level;
    uint64_t *slot = vmm_pte_slot(vm_as_root(as), va, false, &level, NULL);
    CHECK(slot == NULL || level != 1 || !vmm_pte_present(*slot),
          "no PTE present before the first access");

    current->as = as;
    vm_as_activate(as);
    volatile uint64_t *p = (volatile uint64_t *)va;

    uint64_t before = pmm_free_pages();
    uint64_t tp_before = vm_as_table_pages(as);
    p[0] = 0xC0DE5EEDull;                    /* write fault -> fill */
    CHECK(p[0] == 0xC0DE5EEDull,
          "write fault recovered by mapping a zeroed frame");
    CHECK(p[1] == 0,
          "demand-filled pages start zeroed");
    CHECK(vm_as_resident_pages(as) == 1 &&
          pmm_free_pages() == before - 1 -
              (vm_as_table_pages(as) - tp_before),
          "fault allocated exactly one data frame plus its page tables");

    CHECK(p[PAGE_SIZE / 8] == 0,             /* read fault fills page 1 */
          "read fault on the next page demand-fills and reads 0");
    CHECK(vm_as_resident_pages(as) == 2,
          "second page became resident through a read fault");

    /* Unrecoverable faults must stay unrecoverable. */
    CHECK(fault_probe(va + 10 * PAGE_SIZE) == false,
          "fault outside any VMA is not recovered");
    CHECK(!vm_last_fault.guard && !vm_last_fault.write,
          "trap layer classified the out-of-VMA fault (read, no guard)");

    current->as = NULL;
    vm_as_activate(NULL);

    CHECK(vm_munmap(as, va, 4 * PAGE_SIZE), "cleanup munmap");
    vm_as_destroy(as);
}

/* ---- COW clone test: child thread writes through its own AS ---- */

static vm_as_t *cow_parent_as, *cow_child_as;
static volatile uint64_t cow_child_done;
static volatile uint64_t cow_test_va;
static volatile uint64_t cow_shared_read_a, cow_shared_read_b;

static void cow_child_task(uint64_t arg)
{
    (void)arg;
    volatile uint64_t *p = (volatile uint64_t *)cow_test_va;

    /* Reads walk the shared COW mappings. */
    cow_shared_read_a = p[0];
    cow_shared_read_b = p[PAGE_SIZE / 8];

    /* Writes to two DIFFERENT pages trigger two copy-on-write faults. */
    p[0] = 0x1111111111111111ull;
    p[PAGE_SIZE / 8] = 0x2222222222222222ull;

    cow_child_done = 1;
    thread_exit();
}

static void selftest_vm_cow(void)
{
    kprintf("selftest: address-space cloning and copy-on-write\n");

    cow_parent_as = vm_as_create();
    CHECK(cow_parent_as != NULL, "parent address space");

    uint64_t va = vm_mmap(cow_parent_as, 0, 2 * PAGE_SIZE,
                          VM_PROT_READ | VM_PROT_WRITE,
                          VM_VMA_ANON | VM_MAP_POPULATE);
    CHECK(va != 0, "parent RW region");
    cow_test_va = va;

    current->as = cow_parent_as;
    vm_as_activate(cow_parent_as);
    volatile uint64_t *p = (volatile uint64_t *)va;
    p[0] = 0xCAFEF00DDEADBEEFull;
    p[PAGE_SIZE / 8] = 0x0BADC0DE5A5A5A5Aull;   /* second page */

    uint64_t before_clone = pmm_free_pages();
    cow_child_as = vm_as_clone(cow_parent_as);
    CHECK(cow_child_as != NULL, "vm_as_clone succeeds");
    CHECK(pmm_free_pages() ==
              before_clone - vm_as_table_pages(cow_child_as),
          "clone cost is table pages only (no data frames)");

    /* Both sides now hold the same frames, read-only. */
    unsigned level;
    uint64_t *pslot = vmm_pte_slot(vm_as_root(cow_parent_as), va, false,
                                   &level, NULL);
    uint64_t *cslot = vmm_pte_slot(vm_as_root(cow_child_as), va, false,
                                   &level, NULL);
    CHECK(pslot && cslot && level == 1 &&
          vmm_pte_paddr(*pslot) == vmm_pte_paddr(*cslot),
          "parent and child map the same frame");
    CHECK(vmm_pte_paddr(*pslot) != 0 &&
          !(vmm_pte_flags(*pslot) & VM_PROT_WRITE) &&
          !(vmm_pte_flags(*cslot) & VM_PROT_WRITE),
          "both mappings downgraded to read-only");

    /* Child runs with its own address space and COW-copies on write.
     * The AS must be attached before the task can ever run. The child
     * may run entirely inside a timer preemption, so every check uses
     * values that stay meaningful regardless of interleaving. */
    irqstate_t s = irq_save();
    task_t *child = kthread_create(cow_child_task, 0);
    child->as = cow_child_as;
    irq_restore(s);
    CHECK(child != NULL, "child kthread");

    while (!cow_child_done)
        yield();

    CHECK(cow_shared_read_a == 0xCAFEF00DDEADBEEFull &&
          cow_shared_read_b == 0x0BADC0DE5A5A5A5Aull,
          "child read the parent's data through shared mappings");
    CHECK(vm_cow_copies == 2,
          "child's two page writes COW-copied exactly two frames");

    /* Parent is untouched by the child's writes... */
    CHECK(p[0] == 0xCAFEF00DDEADBEEFull &&
          p[PAGE_SIZE / 8] == 0x0BADC0DE5A5A5A5Aull,
          "parent data unchanged after child's COW copies");

    /* ...and its next write is an in-place upgrade (refcount 1). */
    pslot = vmm_pte_slot(vm_as_root(cow_parent_as), va, false, &level, NULL);
    uint64_t pa_before = vmm_pte_paddr(*pslot);
    p[0] = 0x3333333333333333ull;
    pslot = vmm_pte_slot(vm_as_root(cow_parent_as), va, false, &level, NULL);
    CHECK(vmm_pte_paddr(*pslot) == pa_before &&
          (vmm_pte_flags(*pslot) & VM_PROT_WRITE),
          "sole-owner write upgrades in place (no copy)");
    CHECK(p[0] == 0x3333333333333333ull, "parent sees its own write");

    current->as = NULL;
    vm_as_activate(NULL);
    vm_as_destroy(cow_parent_as);
    vm_as_destroy(cow_child_as);
}

static void selftest_vm_guard(void)
{
    kprintf("selftest: guard pages\n");

    vm_as_t *as = vm_as_create();
    CHECK(as != NULL, "address space for guard test");

    uint64_t st = vm_alloc_stack(as, 2, 0x3000000ull);
    CHECK(st == 0x3000000ull + PAGE_SIZE,
          "stack body placed right above the lower guard");

    /* In-bounds pages work normally (demand-filled on first touch).
     * The AS stays active: guard classification looks up the faulting
     * address in the CURRENT address space. */
    current->as = as;
    vm_as_activate(as);
    volatile uint64_t *p = (volatile uint64_t *)st;
    p[0] = 42;
    CHECK(p[0] == 42, "stack body is usable");
    p[PAGE_SIZE / 8] = 43;
    CHECK(p[PAGE_SIZE / 8] == 43, "second stack page is usable");

    /* Guards reject any access, on both sides. */
    CHECK(fault_probe(st - 1) == false, "lower guard page faults");
    CHECK(vm_last_fault.valid && vm_last_fault.guard,
          "lower guard fault classified as guard");
    CHECK(fault_probe(st + 2 * PAGE_SIZE) == false,
          "upper guard page faults");
    CHECK(vm_last_fault.valid && vm_last_fault.guard,
          "upper guard fault classified as guard");
    current->as = NULL;
    vm_as_activate(NULL);

    /* Collision: a second stack overlapping the first is rejected. */
    CHECK(vm_alloc_stack(as, 2, 0x3000000ull) == 0,
          "overlapping stack allocation rejected");

    vm_as_destroy(as);
}

/* ---- scheduler: priority wake order + deadline wakeups ---- */

static wq_t sched_test_wq;
static spinlock_t sched_wake_lk = SPINLOCK_INIT;
static volatile uint64_t sched_parked;
static volatile uint64_t sched_run_idx;
static volatile uint64_t sched_first_run[3];
static volatile uint64_t sched_timed_wake;

static void sched_parked_task(uint64_t arg)
{
    /* Park immediately; the order the scheduler re-runs us after the
     * wake is what the test measures. */
    sched_parked++;
    wq_block(&sched_test_wq, &sched_wake_lk);

    sched_first_run[arg] = ++sched_run_idx;
    thread_exit();
}

static void sched_timed_task(uint64_t arg)
{
    (void)arg;
    thread_sleep_ms(60);
    sched_timed_wake++;
    thread_exit();
}

static void selftest_sched(void)
{
    kprintf("selftest: scheduler priorities and wakeups\n");

    wq_init(&sched_test_wq);
    sched_parked = 0;
    sched_run_idx = 0;

    /* Three parked tasks at distinct priorities: created hi/mid/lo.
     * Each parks at first run; after wake_all, the scheduler must run
     * them strictly by priority, regardless of parking order. */
    task_t *t2 = kthread_create(sched_parked_task, 2);
    task_t *t1 = kthread_create(sched_parked_task, 1);
    task_t *t0 = kthread_create(sched_parked_task, 0);
    CHECK(t0 != NULL && t1 != NULL && t2 != NULL,
          "priority test tasks created");
    thread_set_priority(t0, 0);
    thread_set_priority(t1, 1);
    thread_set_priority(t2, 2);

    while (sched_parked < 3)
        yield();
    CHECK(sched_parked == 3, "all three tasks parked on the wait queue");

    wq_wake_all(&sched_test_wq);
    while (sched_run_idx < 3)
        yield();

    CHECK(sched_first_run[0] == 1 && sched_first_run[1] == 2 &&
          sched_first_run[2] == 3,
          "wake order follows priority (hi, mid, lo)");

    /* Deadline wakeups with no explicit waker. */
    sched_timed_wake = 0;
    CHECK(kthread_create(sched_timed_task, 0) != NULL, "timed task created");
    while (sched_timed_wake == 0)
        yield();
    CHECK(sched_timed_wake == 1, "deadline woke the sleeping task");

    thread_reap();
}

static void kmain_cont(void)
{
    vmm_init();
    console_init();
    kheap_init();
    timer_init();
    thread_init();
    vm_init();

    kprintf("\nJawaOS v0.4.0 (rungs 2-7: memory, exceptions, timers, threads)\n");
    kprintf("  arch:        %s\n", arch_name());
    kprintf("  kprintf:     dec=%d u=%u hex=0x%08x ptr=%p str=%s ch=%c\n",
            -1234, 3000000000u, 0xcafebabe, (void *)&kmain, "ok", '!');

    boot_report();
    selftest_pmm();
    selftest_vmm();
    selftest_kheap();
    selftest_int();
    selftest_timer();
    selftest_thread();
    selftest_vm_as();
    selftest_vm_vma();
    selftest_vm_demand();
    selftest_vm_cow();
    selftest_vm_guard();

    kprintf("\nmemory management online — idling.\n");

    for (;;) {
        cpu_idle();
    }
}
