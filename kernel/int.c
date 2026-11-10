#include <stdint.h>

#include "console.h"
#include "int.h"

static uint64_t brk_count;
static bool probe_armed;

static uint64_t probe_read_leaf(uint64_t addr);
static uint64_t probe_fail_leaf(void);

#if defined(__x86_64__)

struct tf_x86 {
    uint64_t pad;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2, ist3, ist4, ist5, ist6, ist7;
    uint16_t reserved2, iopb;
    uint64_t pad_end;
};

static struct tss64 tss __attribute__((aligned(8)));
static uint64_t df_stack[2048] __attribute__((aligned(16)));

static uint64_t gdt[6] = {
    0x0000000000000000ull,
    0x00af9a000000ffffull,
    0x00cf92000000ffffull,
    0x0000000000000000ull,
    0x0000000000000000ull,
    0x0000000000000000ull,
};

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint16_t flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
};

static struct idt_entry idt[256] __attribute__((aligned(16)));

#define EXTSTUB(n) extern char isr_stub_##n[];
EXTSTUB(0) EXTSTUB(1) EXTSTUB(2) EXTSTUB(3) EXTSTUB(4) EXTSTUB(5)
EXTSTUB(6) EXTSTUB(7) EXTSTUB(8) EXTSTUB(9) EXTSTUB(10) EXTSTUB(11)
EXTSTUB(12) EXTSTUB(13) EXTSTUB(14) EXTSTUB(15) EXTSTUB(16) EXTSTUB(17)
EXTSTUB(18) EXTSTUB(19) EXTSTUB(20) EXTSTUB(21) EXTSTUB(22) EXTSTUB(23)
EXTSTUB(24) EXTSTUB(25) EXTSTUB(26) EXTSTUB(27) EXTSTUB(28) EXTSTUB(29)
EXTSTUB(30) EXTSTUB(31) EXTSTUB(32)

static const void *const stubs[] = {
    isr_stub_0, isr_stub_1, isr_stub_2, isr_stub_3, isr_stub_4, isr_stub_5,
    isr_stub_6, isr_stub_7, isr_stub_8, isr_stub_9, isr_stub_10, isr_stub_11,
    isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15, isr_stub_16,
    isr_stub_17, isr_stub_18, isr_stub_19, isr_stub_20, isr_stub_21,
    isr_stub_22, isr_stub_23, isr_stub_24, isr_stub_25, isr_stub_26,
    isr_stub_27, isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31,
    isr_stub_32,
};

static const char *x86_name(uint64_t v)
{
    static const char *names[] = {
        "divide error", "debug", "NMI", "breakpoint", "overflow",
        "BOUND range exceeded", "invalid opcode", "device not available",
        "double fault", "coprocessor segment overrun", "invalid TSS",
        "segment not present", "stack-segment fault",
        "general protection fault", "page fault", "(reserved)",
        "x87 FPU error", "alignment check", "machine check",
        "SIMD floating-point", "virtualization exception", "(reserved)",
        "(reserved)", "(reserved)", "(reserved)", "(reserved)",
        "(reserved)", "(reserved)", "(reserved)", "(reserved)",
        "(reserved)", "(reserved)", "timer IRQ (PIT)",
    };
    if (v < 33)
        return names[v];
    return "unknown";
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("movq %%cr2, %0" : "=r"(v));
    return v;
}

__attribute__((used, noinline))
static void isr_dispatch(struct tf_x86 *tf)
{
    kprintf("  [trap] v=%llu rip=%016llx err=%llx\n",
            (unsigned long long)tf->vector, (unsigned long long)tf->rip,
            (unsigned long long)tf->error);
    if (tf->vector == 3) {
        brk_count++;
        return;
    }
    if (probe_armed && tf->vector == 14) {
        tf->rax = 1;
        tf->rip = (uint64_t)probe_fail_leaf;
        return;
    }
    int_panic_dump(tf, tf->vector, tf->error,
                   tf->vector == 14 ? read_cr2() : 0);
}

__attribute__((naked, used, section(".text.isr")))
static void isr_stub_table(void)
{
    __asm__ volatile (
        ".macro STUB n err\n"
        "isr_stub_\\n:\n"
        ".if \\err\n"
        "pushq $\\n\n"
        ".else\n"
        "pushq $0\n"
        "pushq $\\n\n"
        ".endif\n"
        "jmp isr_common\n"
        ".endm\n"

        "isr_common:\n"
        "pushq %rax\n"
        "pushq %rbx\n"
        "pushq %rcx\n"
        "pushq %rdx\n"
        "pushq %rsi\n"
        "pushq %rdi\n"
        "pushq %rbp\n"
        "pushq %r8\n"
        "pushq %r9\n"
        "pushq %r10\n"
        "pushq %r11\n"
        "pushq %r12\n"
        "pushq %r13\n"
        "pushq %r14\n"
        "pushq %r15\n"
        "subq $8, %rsp\n"
        "movq %rsp, %rdi\n"
        "call isr_dispatch\n"
        "addq $8, %rsp\n"
        "popq %r15\n"
        "popq %r14\n"
        "popq %r13\n"
        "popq %r12\n"
        "popq %r11\n"
        "popq %r10\n"
        "popq %r9\n"
        "popq %r8\n"
        "popq %rbp\n"
        "popq %rdi\n"
        "popq %rsi\n"
        "popq %rdx\n"
        "popq %rcx\n"
        "popq %rbx\n"
        "popq %rax\n"
        "addq $16, %rsp\n"
        "iretq\n"

        "STUB 0 0\n"
        "STUB 1 0\n"
        "STUB 2 0\n"
        "STUB 3 0\n"
        "STUB 4 0\n"
        "STUB 5 0\n"
        "STUB 6 0\n"
        "STUB 7 0\n"
        "STUB 8 1\n"
        "STUB 9 0\n"
        "STUB 10 1\n"
        "STUB 11 1\n"
        "STUB 12 1\n"
        "STUB 13 1\n"
        "STUB 14 1\n"
        "STUB 15 0\n"
        "STUB 16 0\n"
        "STUB 17 1\n"
        "STUB 18 0\n"
        "STUB 19 0\n"
        "STUB 20 0\n"
        "STUB 21 0\n"
        "STUB 22 0\n"
        "STUB 23 0\n"
        "STUB 24 0\n"
        "STUB 25 0\n"
        "STUB 26 0\n"
        "STUB 27 0\n"
        "STUB 28 0\n"
        "STUB 29 0\n"
        "STUB 30 0\n"
        "STUB 31 0\n"
        "STUB 32 0\n"
    );
}

static void idt_set(int v, uint64_t handler, int ist)
{
    idt[v].off_lo = handler & 0xffff;
    idt[v].sel = 0x08;
    idt[v].flags = (ist & 7) | (0x8f << 8);
    idt[v].off_mid = (handler >> 16) & 0xffff;
    idt[v].off_hi = (uint32_t)(handler >> 32);
    idt[v].zero = 0;
}

void int_init(void)
{
    static const struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };

    tss.ist1 = (uint64_t)(df_stack + 2048);
    tss.iopb = sizeof(struct tss64);

    uint64_t tss_base = (uint64_t)&tss;
    uint32_t lo = tss_base & 0xffffffffu, hi = tss_base >> 32;
    uint32_t lim = sizeof(struct tss64) - 1;   /* must be >= 0x67 */
    gdt[3] = (uint64_t)lim | ((lo & 0xffff) << 16) |
             (((uint64_t)(lo >> 16) & 0xff) << 32) |
             (((uint64_t)(lo >> 24) & 0xff) << 56) | (0x89ull << 40);
    gdt[4] = hi;

    for (int v = 0; v < 33; v++)
        idt_set(v, (uint64_t)stubs[v], v == 8 ? 1 : 0);

    static const struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = { sizeof(idt) - 1, (uint64_t)idt };

    __asm__ volatile (
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        : "m"(gdtr)
        : "rax", "memory");

    __asm__ volatile ("lidt %0" :: "m"(idtr));
    __asm__ volatile ("ltr %w0" :: "rm"((uint16_t)0x18));
}

void int_dump_frame(const void *frame)
{
    const struct tf_x86 *tf = frame;

    kprintf("  rip=%016llx cs=%04llx rflags=%016llx\n",
            (unsigned long long)tf->rip, (unsigned long long)tf->cs,
            (unsigned long long)tf->rflags);
    kprintf("  rsp=%016llx ss=%04llx rbp=%016llx\n",
            (unsigned long long)tf->rsp, (unsigned long long)tf->ss,
            (unsigned long long)tf->rbp);
    kprintf("  rax=%016llx rbx=%016llx rcx=%016llx\n",
            (unsigned long long)tf->rax, (unsigned long long)tf->rbx,
            (unsigned long long)tf->rcx);
    kprintf("  rdx=%016llx rsi=%016llx rdi=%016llx\n",
            (unsigned long long)tf->rdx, (unsigned long long)tf->rsi,
            (unsigned long long)tf->rdi);
    kprintf("  r8 =%016llx r9 =%016llx r10=%016llx\n",
            (unsigned long long)tf->r8, (unsigned long long)tf->r9,
            (unsigned long long)tf->r10);
    kprintf("  r11=%016llx r12=%016llx r13=%016llx\n",
            (unsigned long long)tf->r11, (unsigned long long)tf->r12,
            (unsigned long long)tf->r13);
    kprintf("  r14=%016llx r15=%016llx\n",
            (unsigned long long)tf->r14, (unsigned long long)tf->r15);
}

const char *int_exception_name(uint64_t vector)
{
    return x86_name(vector);
}

void int_panic_dump(const void *frame, uint64_t vector, uint64_t detail,
                    uint64_t fault_addr)
{
    kprintf("\n\n*** KERNEL PANIC: unhandled exception ***\n");
    kprintf("vector %llu (%s)\n", (unsigned long long)vector,
            int_exception_name(vector));

    if (vector == 14) {
        kprintf("  CR2 (fault address): 0x%016llx\n",
                (unsigned long long)fault_addr);
        kprintf("  error code 0x%llx: %s %s %s%s%s%s\n",
                (unsigned long long)detail,
                detail & 1 ? "present" : "not-present",
                detail & 2 ? "write" : "read",
                detail & 4 ? "user" : "kernel",
                detail & 8 ? " reserved-bit-violation" : "",
                detail & 16 ? " instruction-fetch" : "",
                detail & 32 ? " protection-key" : "");
    } else if (vector == 8 || vector == 10 || vector == 11 ||
               vector == 12 || vector == 13 || vector == 17) {
        kprintf("  error code 0x%llx\n", (unsigned long long)detail);
    }

    int_dump_frame(frame);
    kprintf("\nCPU halted.\n");
    for (;;)
        __asm__ volatile ("cli\n\thlt");
}

__attribute__((naked, noinline))
static uint64_t probe_read_leaf(uint64_t addr __attribute__((unused)))
{
    __asm__ volatile ("movq (%rdi), %rax\n\txorq %rax, %rax\n\tret\n");
}

__attribute__((naked, noinline, used))
static uint64_t probe_fail_leaf(void)
{
    __asm__ volatile ("movq $1, %rax\n\tret\n");
}

bool int_breakpoint_selftest(void)
{
    brk_count = 0;
    __asm__ volatile ("int3");
    return brk_count == 1;
}

#elif defined(__aarch64__)

struct tf_a64 {
    uint64_t meta[8];
    uint64_t x30, pad;
    uint64_t x28, x29, x26, x27, x24, x25, x22, x23, x20, x21;
    uint64_t x18, x19, x16, x17, x14, x15, x12, x13, x10, x11;
    uint64_t x8, x9, x6, x7, x4, x5, x2, x3, x0, x1;
};

#define ESR_EC(esr) (((esr) >> 26) & 0x3f)

__attribute__((naked, used, section(".text.vectors"), aligned(2048)))
static void vector_table(void)
{
    __asm__ volatile (
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
        "b exc_common\n.balign 128\n"
    );
}

__attribute__((naked, noinline, used))
static void exc_common(void)
{
    __asm__ volatile (
        "stp x0, x1, [sp, #-16]!\n"
        "stp x2, x3, [sp, #-16]!\n"
        "stp x4, x5, [sp, #-16]!\n"
        "stp x6, x7, [sp, #-16]!\n"
        "stp x8, x9, [sp, #-16]!\n"
        "stp x10, x11, [sp, #-16]!\n"
        "stp x12, x13, [sp, #-16]!\n"
        "stp x14, x15, [sp, #-16]!\n"
        "stp x16, x17, [sp, #-16]!\n"
        "stp x18, x19, [sp, #-16]!\n"
        "stp x20, x21, [sp, #-16]!\n"
        "stp x22, x23, [sp, #-16]!\n"
        "stp x24, x25, [sp, #-16]!\n"
        "stp x26, x27, [sp, #-16]!\n"
        "stp x28, x29, [sp, #-16]!\n"
        "stp x30, x30, [sp, #-16]!\n"
        "sub sp, sp, #64\n"
        "mov x0, sp\n"
        "bl exc_dispatch\n"
        "add sp, sp, #64\n"
        "ldp x30, x30, [sp], #16\n"
        "ldp x28, x29, [sp], #16\n"
        "ldp x26, x27, [sp], #16\n"
        "ldp x24, x25, [sp], #16\n"
        "ldp x22, x23, [sp], #16\n"
        "ldp x20, x21, [sp], #16\n"
        "ldp x18, x19, [sp], #16\n"
        "ldp x16, x17, [sp], #16\n"
        "ldp x14, x15, [sp], #16\n"
        "ldp x12, x13, [sp], #16\n"
        "ldp x10, x11, [sp], #16\n"
        "ldp x8, x9, [sp], #16\n"
        "ldp x6, x7, [sp], #16\n"
        "ldp x4, x5, [sp], #16\n"
        "ldp x2, x3, [sp], #16\n"
        "ldp x0, x1, [sp], #16\n"
        "eret\n"
    );
}

static uint64_t tf_gpr(const struct tf_a64 *tf, int n)
{
    switch (n) {
    case 0: return tf->x0;
    case 1: return tf->x1;
    case 2: return tf->x2;
    case 3: return tf->x3;
    case 4: return tf->x4;
    case 5: return tf->x5;
    case 6: return tf->x6;
    case 7: return tf->x7;
    case 8: return tf->x8;
    case 9: return tf->x9;
    case 10: return tf->x10;
    case 11: return tf->x11;
    case 12: return tf->x12;
    case 13: return tf->x13;
    case 14: return tf->x14;
    case 15: return tf->x15;
    case 16: return tf->x16;
    case 17: return tf->x17;
    case 18: return tf->x18;
    case 19: return tf->x19;
    case 20: return tf->x20;
    case 21: return tf->x21;
    case 22: return tf->x22;
    case 23: return tf->x23;
    case 24: return tf->x24;
    case 25: return tf->x25;
    case 26: return tf->x26;
    case 27: return tf->x27;
    case 28: return tf->x28;
    case 29: return tf->x29;
    case 30: return tf->x30;
    default: return 0;
    }
}

static const char *a64_name(uint64_t ec)
{
    switch (ec) {
    case 0x00: return "unknown exception";
    case 0x01: return "trapped WFI/WFE";
    case 0x15: return "SVC from AArch64";
    case 0x18: return "trapped MSR/MRS";
    case 0x20: return "instruction abort (current EL)";
    case 0x21: return "instruction abort (lower EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "data abort (current EL)";
    case 0x25: return "data abort (lower EL)";
    case 0x26: return "SP alignment fault";
    case 0x2f: return "SError (async external abort)";
    case 0x3c: return "BRK breakpoint";
    default:   return "other exception class";
    }
}

__attribute__((used, noinline))
static void exc_dispatch(struct tf_a64 *tf)
{
    uint64_t esr, far, elr, spsr;

    __asm__ volatile ("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile ("mrs %0, elr_el1" : "=r"(elr));
    __asm__ volatile ("mrs %0, spsr_el1" : "=r"(spsr));

    tf->meta[0] = esr;
    tf->meta[1] = far;
    tf->meta[2] = elr;
    tf->meta[3] = spsr;

    kprintf("  [trap] esr=%016llx far=%016llx elr=%016llx\n",
            (unsigned long long)esr, (unsigned long long)far,
            (unsigned long long)elr);

    uint64_t ec = ESR_EC(esr);

    if (esr == 0) {
        // IRQ/FIQ have a zero ESR; interrupts stay masked for now, so this
        // should not happen.
    } else if (ec == 0x3c || ec == 0x15) {
        brk_count++;
        if (ec == 0x3c)
            tf->meta[2] = elr + 4;   /* BRK: ELR points AT the instruction;
                                        SVC: ELR already points past it */
    } else if (probe_armed &&
               (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25)) {
        tf->x0 = 1;
        tf->meta[2] = (uint64_t)probe_fail_leaf;
    } else {
        int_panic_dump(tf, ec, esr, far);
    }

    __asm__ volatile ("msr elr_el1, %0" :: "r"(tf->meta[2]) : "memory");
    __asm__ volatile ("msr spsr_el1, %0" :: "r"(tf->meta[3]) : "memory");
}

void int_init(void)
{
    uint64_t vbar = (uint64_t)vector_table;
    __asm__ volatile ("msr vbar_el1, %0\n\tisb" :: "r"(vbar) : "memory");
    kprintf("  int: vbar=0x%016llx (align 0x%llx)\n",
            (unsigned long long)vbar,
            (unsigned long long)(vbar & 0x7ff));
}

void int_dump_frame(const void *frame)
{
    const struct tf_a64 *tf = frame;

    kprintf("  elr=%016llx spsr=%016llx\n",
            (unsigned long long)tf->meta[2],
            (unsigned long long)tf->meta[3]);
    for (int i = 0; i < 30; i += 3)
        kprintf("  x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n",
                i, (unsigned long long)tf_gpr(tf, i),
                i + 1, (unsigned long long)tf_gpr(tf, i + 1),
                i + 2, (unsigned long long)tf_gpr(tf, i + 2));
    kprintf("  x30=%016llx\n", (unsigned long long)tf->x30);
}

const char *int_exception_name(uint64_t vector)
{
    return a64_name(vector);
}

void int_panic_dump(const void *frame, uint64_t vector, uint64_t detail,
                    uint64_t fault_addr)
{
    const struct tf_a64 *tf = frame;
    uint64_t esr = tf->meta[0];

    (void)detail;
    kprintf("\n\n*** KERNEL PANIC: unhandled exception ***\n");
    kprintf("EC 0x%llx (%s)\n", (unsigned long long)vector,
            int_exception_name(vector));
    kprintf("  ESR_EL1 = 0x%016llx\n", (unsigned long long)esr);
    kprintf("  FAR_EL1 (fault address) = 0x%016llx\n",
            (unsigned long long)fault_addr);
    if (ESR_EC(esr) == 0x24 || ESR_EC(esr) == 0x25)
        kprintf("  access: %s\n", esr & (1 << 6) ? "write" : "read");

    int_dump_frame(frame);
    kprintf("\nCPU halted.\n");
    for (;;)
        __asm__ volatile ("wfi");
}

__attribute__((naked, noinline))
static uint64_t probe_read_leaf(uint64_t addr __attribute__((unused)))
{
    __asm__ volatile ("ldr x0, [x0]\n\tmov x0, #0\n\tret\n");
}

__attribute__((naked, noinline, used))
static uint64_t probe_fail_leaf(void)
{
    __asm__ volatile ("mov x0, #1\n\tret\n");
}

bool int_breakpoint_selftest(void)
{
    brk_count = 0;
    __asm__ volatile ("svc #0");
    return brk_count == 1;
}

#elif defined(__riscv)

struct tf_rv {
    uint64_t gpr[31];
    uint64_t sepc, sstatus, scause, stval;
};

__attribute__((naked, noinline, used))
static void trap_entry(void)
{
    __asm__ volatile (
        "addi sp, sp, -280\n"
        "sd x5, 32(sp)\n"
        "addi t0, sp, 280\n"
        "sd x1, 0(sp)\n"
        "sd t0, 8(sp)\n"
        "sd x3, 16(sp)\n"
        "sd x4, 24(sp)\n"
        "sd x6, 40(sp)\n"
        "sd x7, 48(sp)\n"
        "sd x8, 56(sp)\n"
        "sd x9, 64(sp)\n"
        "sd x10, 72(sp)\n"
        "sd x11, 80(sp)\n"
        "sd x12, 88(sp)\n"
        "sd x13, 96(sp)\n"
        "sd x14, 104(sp)\n"
        "sd x15, 112(sp)\n"
        "sd x16, 120(sp)\n"
        "sd x17, 128(sp)\n"
        "sd x18, 136(sp)\n"
        "sd x19, 144(sp)\n"
        "sd x20, 152(sp)\n"
        "sd x21, 160(sp)\n"
        "sd x22, 168(sp)\n"
        "sd x23, 176(sp)\n"
        "sd x24, 184(sp)\n"
        "sd x25, 192(sp)\n"
        "sd x26, 200(sp)\n"
        "sd x27, 208(sp)\n"
        "sd x28, 216(sp)\n"
        "sd x29, 224(sp)\n"
        "sd x30, 232(sp)\n"
        "sd x31, 240(sp)\n"
        "csrr t0, sepc\n"
        "sd t0, 248(sp)\n"
        "csrr t0, sstatus\n"
        "sd t0, 256(sp)\n"
        "csrr t0, scause\n"
        "sd t0, 264(sp)\n"
        "csrr t0, stval\n"
        "sd t0, 272(sp)\n"
        "mv a0, sp\n"
        "call trap_dispatch\n"
        "ld t0, 248(sp)\n"
        "csrw sepc, t0\n"
        "ld x1, 0(sp)\n"
        "ld x3, 16(sp)\n"
        "ld x4, 24(sp)\n"
        "ld x5, 32(sp)\n"
        "ld x6, 40(sp)\n"
        "ld x7, 48(sp)\n"
        "ld x8, 56(sp)\n"
        "ld x9, 64(sp)\n"
        "ld x10, 72(sp)\n"
        "ld x11, 80(sp)\n"
        "ld x12, 88(sp)\n"
        "ld x13, 96(sp)\n"
        "ld x14, 104(sp)\n"
        "ld x15, 112(sp)\n"
        "ld x16, 120(sp)\n"
        "ld x17, 128(sp)\n"
        "ld x18, 136(sp)\n"
        "ld x19, 144(sp)\n"
        "ld x20, 152(sp)\n"
        "ld x21, 160(sp)\n"
        "ld x22, 168(sp)\n"
        "ld x23, 176(sp)\n"
        "ld x24, 184(sp)\n"
        "ld x25, 192(sp)\n"
        "ld x26, 200(sp)\n"
        "ld x27, 208(sp)\n"
        "ld x28, 216(sp)\n"
        "ld x29, 224(sp)\n"
        "ld x30, 232(sp)\n"
        "ld x31, 240(sp)\n"
        "ld sp, 8(sp)\n"
        "sret\n"
    );
}

static const char *rv_name(uint64_t scause)
{
    switch (scause) {
    case 0: return "instruction address misaligned";
    case 1: return "instruction access fault";
    case 2: return "illegal instruction";
    case 3: return "breakpoint";
    case 4: return "load address misaligned";
    case 5: return "load access fault";
    case 6: return "store address misaligned";
    case 7: return "store access fault";
    case 8: return "environment call from U-mode";
    case 9: return "environment call from S-mode";
    case 12: return "instruction page fault";
    case 13: return "load page fault";
    case 15: return "store page fault";
    default:
        if ((int64_t)scause < 0)
            return "supervisor interrupt";
        return "unknown exception";
    }
}

__attribute__((used, noinline))
static void trap_dispatch(struct tf_rv *tf)
{
    uint64_t scause = tf->scause;

    if (scause == 3) {
        brk_count++;
        tf->sepc += 4;
        return;
    }
    if (probe_armed &&
        (scause == 4 || scause == 5 || scause == 6 || scause == 7 ||
         scause == 13 || scause == 15)) {
        tf->gpr[9] = 1;
        tf->sepc = (uint64_t)probe_fail_leaf;
        return;
    }
    int_panic_dump(tf, scause, 0, tf->stval);
}

void int_init(void)
{
    uint64_t stvec = (uint64_t)trap_entry;
    __asm__ volatile ("csrw stvec, %0" :: "r"(stvec) : "memory");
}

void int_dump_frame(const void *frame)
{
    const struct tf_rv *tf = frame;

    kprintf("  sepc=%016llx sstatus=%016llx\n",
            (unsigned long long)tf->sepc, (unsigned long long)tf->sstatus);
    for (int i = 0; i < 30; i += 3)
        kprintf("  x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n",
                i + 1, (unsigned long long)tf->gpr[i],
                i + 2, (unsigned long long)tf->gpr[i + 1],
                i + 3, (unsigned long long)tf->gpr[i + 2]);
    kprintf("  x31=%016llx\n", (unsigned long long)tf->gpr[30]);
}

const char *int_exception_name(uint64_t vector)
{
    return rv_name(vector);
}

void int_panic_dump(const void *frame, uint64_t vector, uint64_t detail,
                    uint64_t fault_addr)
{
    (void)detail;
    kprintf("\n\n*** KERNEL PANIC: unhandled exception ***\n");
    kprintf("scause %llu (%s)\n", (unsigned long long)vector,
            int_exception_name(vector));
    kprintf("  stval (fault address) = 0x%016llx\n",
            (unsigned long long)fault_addr);

    int_dump_frame(frame);
    kprintf("\nCPU halted.\n");
    for (;;)
        __asm__ volatile ("wfi");
}

__attribute__((naked, noinline))
static uint64_t probe_read_leaf(uint64_t addr __attribute__((unused)))
{
    __asm__ volatile ("ld a0, 0(a0)\n\tli a0, 0\n\tret\n");
}

__attribute__((naked, noinline, used))
static uint64_t probe_fail_leaf(void)
{
    __asm__ volatile ("li a0, 1\n\tret\n");
}

bool int_breakpoint_selftest(void)
{
    brk_count = 0;
    __asm__ volatile ("ebreak");
    return brk_count == 1;
}

#else
#error "unsupported architecture"
#endif

bool fault_probe(uint64_t addr)
{
    probe_armed = true;
    uint64_t r = probe_read_leaf(addr);
    probe_armed = false;
    return r == 0;
}
