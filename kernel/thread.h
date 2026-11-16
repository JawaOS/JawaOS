#ifndef JAWA_THREAD_H
#define JAWA_THREAD_H

#include <stdint.h>
#include <stdbool.h>

struct vm_as;

typedef struct task {
    uint64_t sp;
    struct task *next;
    struct vm_as *as;   /* process address space; NULL = kernel-only */
} task_t;

extern task_t *current;

void thread_init(void);
task_t *kthread_create(void (*fn)(uint64_t), uint64_t arg);
void yield(void);
void schedule(void);
void thread_exit(void) __attribute__((noreturn));

#endif
