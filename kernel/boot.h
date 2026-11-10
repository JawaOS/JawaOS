#ifndef JAWA_BOOT_H
#define JAWA_BOOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BOOT_MEMMAP_MAX 128

struct boot_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct boot_fb {
    uint64_t phys_addr;
    uint64_t width, height;
    uint64_t pitch;
    uint32_t bpp;
    uint32_t memory_model;
    uint8_t red_size, red_shift;
    uint8_t green_size, green_shift;
    uint8_t blue_size, blue_shift;
    uint64_t phys_end;
};

struct bootinfo {

    uint64_t hhdm_offset;

    uint64_t memmap_count;
    struct boot_memmap_entry memmap[BOOT_MEMMAP_MAX];

    uint64_t kernel_phys_base;
    uint64_t kernel_virt_base;

    uint8_t rsdp_copy[36];
    void *rsdp;

#define BOOT_DTB_MAX (1u << 20)
    uint8_t dtb_copy[BOOT_DTB_MAX];
    uint64_t dtb_size;
    void *dtb;

    struct boot_fb fb;
    bool fb_present;
};

extern struct bootinfo bootinfo;

void boot_init(void);

static inline uint64_t boot_phys_to_virt(uint64_t phys)
{
    return phys + bootinfo.hhdm_offset;
}

static inline uint64_t boot_virt_to_phys(uint64_t virt)
{
    return virt - bootinfo.hhdm_offset;
}

const char *boot_memmap_type_name(uint64_t type);

#endif
