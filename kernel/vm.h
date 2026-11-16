#ifndef JAWA_VM_H
#define JAWA_VM_H

#include <stdint.h>
#include <stdbool.h>

/* Process-level virtual memory on top of the raw VMM.
 *
 * A vm_as owns one user root table (lower VA half only — the higher half
 * is shared kernel space) and, later, the VMA list that describes it.
 * Kernel threads run with as == NULL and keep the kernel root installed.
 */

typedef struct vm_as vm_as_t;

/* VMA / mmap flags (OR-ed with VM_PROT_* permissions). */
#define VM_VMA_ANON     0x100u  /* anonymous demand-zero memory        */
#define VM_VMA_GUARD    0x200u  /* guard: any access is a fatal fault  */
#define VM_MAP_POPULATE 0x400u  /* map every page eagerly at mmap time */
#define VM_MAP_FIXED    0x800u  /* hint is mandatory, not preferred    */

/* Lower bound for anonymous mappings: the first 16 MiB of the user half
 * stay unmapped so NULL and near-NULL dereferences always fault. */
#define VM_USER_BASE 0x0000000001000000ull

void vm_init(void);

vm_as_t *vm_as_create(void);

void vm_as_destroy(vm_as_t *as);

/* Install as on this CPU; NULL restores the kernel root. */
void vm_as_activate(vm_as_t *as);

vm_as_t *vm_as_current(void);

uint64_t vm_as_root(vm_as_t *as);

uint64_t vm_as_table_pages(vm_as_t *as);

/* Create a memory-mapped region. hint==0: first-fit placement above
 * VM_USER_BASE. hint!=0: preferred base (mandatory with VM_MAP_FIXED).
 * Returns the region base or 0 on failure. Without VM_MAP_POPULATE the
 * region is only registered — pages materialise on demand. */
uint64_t vm_mmap(vm_as_t *as, uint64_t hint, uint64_t len,
                 unsigned prot, unsigned vflags);

/* Unmap [addr, addr+len), freeing resident pages. Holes inside the
 * range are fine; returns false if nothing overlapped. */
bool vm_munmap(vm_as_t *as, uint64_t addr, uint64_t len);

/* Clone an address space: same VMAs, same data — writable pages become
 * shared copy-on-write (refcounted frames, downgraded to read-only in
 * both spaces), read-only pages are shared as-is. Returns NULL on
 * failure (the source is left functional). */
vm_as_t *vm_as_clone(vm_as_t *src);

/* Allocate a guarded stack: [guard 4 KiB][npages RW][guard 4 KiB].
 * hint is the address of the lower guard page (0: chosen first-fit).
 * Returns the base of the usable stack area (the first RW page), or 0
 * on failure. Any access to a guard page is an unrecoverable fault
 * (vm_last_fault.guard is set when it happens). */
uint64_t vm_alloc_stack(vm_as_t *as, uint64_t npages, uint64_t hint);

/* Number of resident (mapped) pages owned by the address space. */
uint64_t vm_as_resident_pages(vm_as_t *as);

/* ---- page faults ---- */

/* What the most recent page fault looked like (set on every fault the
 * trap layer sees, resolved or not). */
struct vm_fault_info {
    bool valid;
    bool write;
    bool exec;
    bool guard;     /* faulted inside a VM_VMA_GUARD region */
    uint64_t va;
};

extern struct vm_fault_info vm_last_fault;

/* Classify a fault against the current address space. Always records
 * vm_last_fault; never resolves anything. */
void vm_fault_record(uint64_t va, bool write, bool exec);

/* Try to recover a page fault in the current address space by mapping
 * the page on demand (anonymous zero-fill). Returns false when the
 * fault is not recoverable: no VMA, permission violation, guard page,
 * already-resident mapping, or out of memory. */
bool vm_handle_fault(uint64_t va, bool write, bool exec);

extern uint64_t vm_cow_copies;  /* COW copy count (tests/diagnostics) */

#endif
