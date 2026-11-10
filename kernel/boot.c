#include <stdint.h>
#include <stddef.h>

#include <limine.h>

#include "boot.h"
#include "console.h"
#include "pmm.h"
#include "string.h"

#ifndef LIMINE_MEMMAP_RESERVED_MAPPED
#define LIMINE_MEMMAP_RESERVED_MAPPED 8
#endif

struct bootinfo bootinfo;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

const char *boot_memmap_type_name(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "ACPI NVS";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad memory";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case LIMINE_MEMMAP_KERNEL_AND_MODULES:     return "kernel + modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved (mapped)";
    default:                                   return "unknown";
    }
}

void boot_init(void)
{

    if (hhdm_request.response == NULL)
        panic("boot: Limine did not provide an HHDM offset");
    bootinfo.hhdm_offset = hhdm_request.response->offset;

    if (memmap_request.response == NULL || memmap_request.response->entry_count == 0)
        panic("boot: Limine did not provide a memory map");

    uint64_t n = memmap_request.response->entry_count;
    if (n > BOOT_MEMMAP_MAX)
        panic("boot: memory map has %llu entries, snapshot cap is %d",
              (unsigned long long)n, BOOT_MEMMAP_MAX);

    for (uint64_t i = 0; i < n; i++) {
        struct limine_memmap_entry *e = memmap_request.response->entries[i];
        bootinfo.memmap[i].base = e->base;
        bootinfo.memmap[i].length = e->length;
        bootinfo.memmap[i].type = e->type;
    }
    bootinfo.memmap_count = n;

    if (kernel_address_request.response == NULL)
        panic("boot: Limine did not provide the executable address");
    bootinfo.kernel_phys_base = kernel_address_request.response->physical_base;
    bootinfo.kernel_virt_base = kernel_address_request.response->virtual_base;

    bootinfo.rsdp = NULL;
    if (rsdp_request.response != NULL &&
        rsdp_request.response->address != NULL) {
        const uint8_t *src = (const uint8_t *)rsdp_request.response->address;

        memcpy(bootinfo.rsdp_copy, src, 20);
        if (bootinfo.rsdp_copy[15] >= 2)
            memcpy(bootinfo.rsdp_copy + 20, src + 20, 16);
        bootinfo.rsdp = bootinfo.rsdp_copy;
    }

    bootinfo.dtb = NULL;
    bootinfo.dtb_size = 0;
    if (dtb_request.response != NULL && dtb_request.response->dtb_ptr != NULL) {
        const uint8_t *src = (const uint8_t *)dtb_request.response->dtb_ptr;
        uint64_t size = ((uint64_t)src[4] << 24) | ((uint64_t)src[5] << 16) |
                        ((uint64_t)src[6] << 8)  |  (uint64_t)src[7];

        if (size < 40 || size > BOOT_DTB_MAX)
            panic("boot: device tree blob has bogus size %llu",
                  (unsigned long long)size);
        memcpy(bootinfo.dtb_copy, src, size);
        bootinfo.dtb = bootinfo.dtb_copy;
        bootinfo.dtb_size = size;
    }

    bootinfo.fb_present = false;
    bootinfo.fb.phys_end = 0;
    if (framebuffer_request.response != NULL &&
        framebuffer_request.response->framebuffer_count >= 1 &&
        framebuffer_request.response->framebuffers[0] != NULL) {
        struct limine_framebuffer *fb =
            framebuffer_request.response->framebuffers[0];
        uint64_t fb_phys = boot_virt_to_phys((uint64_t)fb->address);

        bootinfo.fb.phys_addr    = fb_phys;
        bootinfo.fb.width        = fb->width;
        bootinfo.fb.height       = fb->height;
        bootinfo.fb.pitch        = fb->pitch;
        bootinfo.fb.bpp          = (uint32_t)fb->bpp;
        bootinfo.fb.memory_model = (uint32_t)fb->memory_model;
        bootinfo.fb.red_size     = fb->red_mask_size;
        bootinfo.fb.red_shift    = fb->red_mask_shift;
        bootinfo.fb.green_size   = fb->green_mask_size;
        bootinfo.fb.green_shift  = fb->green_mask_shift;
        bootinfo.fb.blue_size    = fb->blue_mask_size;
        bootinfo.fb.blue_shift   = fb->blue_mask_shift;
        bootinfo.fb.phys_end     =
            page_align_up(fb_phys + fb->pitch * fb->height);
        bootinfo.fb_present = true;
    }
}
