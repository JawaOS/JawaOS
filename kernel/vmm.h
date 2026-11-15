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

/* ---- per-address-space page table API (user half of a root) ----
 *
 * The kernel root installed at boot stays the owner of the higher half.
 * Additional roots (one per process address space) only ever carry user
 * mappings in their low half; the higher half is copied from the kernel
 * root at creation time (x86_64: shared PML4 entries 256..511,
 * riscv64: same upper-half convention, aarch64: separate TTBR0 with
 * nothing to copy).
 */

unsigned vmm_levels_count(void);

unsigned vmm_user_levels(void);

uint64_t vmm_kernel_root(void);

uint64_t vmm_user_va_limit(void);

/* Walk root_pa for va down to the level-1 slot. With create=true the full
 * path is materialised (level-1 slot always returned; *out_created, when
 * non-NULL, counts the table pages allocated on this walk). With
 * create=false the walk stops at the first non-present entry: the
 * returned slot is the one where it stopped and *out_level is its level
 * (caller must check presence/leafness). A huge page in the path returns
 * that slot/level. */
uint64_t *vmm_pte_slot(uint64_t root_pa, uint64_t va, bool create,
                       unsigned *out_level, unsigned *out_created);

bool vmm_pte_present(uint64_t e);

bool vmm_pte_is_leaf(uint64_t e, unsigned level);

uint64_t vmm_pte_paddr(uint64_t e);

unsigned vmm_pte_flags(uint64_t e);

void vmm_pte_write_leaf(uint64_t *slot, uint64_t paddr, unsigned flags);

void vmm_pte_clear(uint64_t *slot);

/* Local (this-CPU) invalidations. */
void vmm_tlb_inval(uint64_t va);

void vmm_tlb_flush_local(void);

void vmm_copy_kernel_half(uint64_t dst_root_pa);

/* Free every table page reachable from the user half of root_pa
 * (root included). Leaf data frames are left alone — the vm layer
 * releases those through its VMA/refcount bookkeeping. */
void vmm_free_user_tables(uint64_t root_pa);

#endif
