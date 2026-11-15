#ifndef JAWA_VM_H
#define JAWA_VM_H

#include <stdint.h>
#include <stdbool.h>

/* Process-level virtual memory on top of the raw VMM.
 *
 * A vm_as owns one user root table (lower VA half only — the higher half
 * is shared kernel space). Kernel threads run with as == NULL and keep
 * the kernel root installed.
 */

typedef struct vm_as vm_as_t;

void vm_init(void);

vm_as_t *vm_as_create(void);

void vm_as_destroy(vm_as_t *as);

/* Install as on this CPU; NULL restores the kernel root. */
void vm_as_activate(vm_as_t *as);

vm_as_t *vm_as_current(void);

uint64_t vm_as_root(vm_as_t *as);

uint64_t vm_as_table_pages(vm_as_t *as);

#endif
