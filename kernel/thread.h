#ifndef JAWA_THREAD_H
#define JAWA_THREAD_H

#include <stdint.h>
#include <stdbool.h>

typedef struct task {
    uint64_t sp;
    struct task *next;
} task_t;

extern task_t *current;

void thread_init(void);
void kthread_create(void (*fn)(uint64_t), uint64_t arg);
void yield(void);
void schedule(void);
void thread_exit(void) __attribute__((noreturn));

#endif
