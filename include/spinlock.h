#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

uint64_t irq_save_disable(void);
void irq_restore(uint64_t flags);
void spinlock_init(spinlock_t* lock);
void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);
uint64_t spin_lock_irqsave(spinlock_t* lock);
void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags);
void kernel_lock_enter(void);
void kernel_lock_exit(void);
int kernel_lock_held(void);
void kernel_yield(void);

#endif
