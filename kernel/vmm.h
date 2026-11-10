#ifndef JAWA_VMM_H
#define JAWA_VMM_H

#include <stdint.h>
#include <stdbool.h>

#define VM_PROT_READ  0x1
#define VM_PROT_WRITE 0x2
#define VM_PROT_EXEC  0x4
#define VM_PROT_USER  0x8
#define VM_PROT_DEVICE 0x10

#define KHEAP_BASE      0xffffffff40000000ull
#define KHEAP_MAX_SIZE  0x04000000ull

#define VMM_SCRATCH_BASE 0xffffffff20000000ull
#define VMM_SCRATCH_SIZE 0x00200000ull

#define VMM_DEVICE_BASE 0xffffffff00000000ull
#define VMM_DEVICE_SIZE 0x08000000ull

void vmm_init(void);

void vm_map(uint64_t vaddr, uint64_t paddr, unsigned flags);

bool vm_unmap(uint64_t vaddr);

bool vmm_query(uint64_t vaddr, uint64_t *paddr, unsigned *flags);

void *vmm_map_device(uint64_t phys, uint64_t size);

uint64_t vmm_root_phys(void);

uint64_t vmm_hhdm_limit(void);

const char *vmm_paging_mode_name(void);

uint64_t vmm_table_pages(void);

#endif
