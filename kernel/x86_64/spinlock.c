#include "spinlock.h"
#include "task.h"

static spinlock_t g_kernel_lock;

void schedule(void);

uint64_t irq_save_disable(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" : : : "memory");
    }
}

void spinlock_init(spinlock_t* lock) {
    if (!lock) return;
    lock->locked = 0;
}

void spin_lock(spinlock_t* lock) {
    if (!lock) return;
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

void spin_unlock(spinlock_t* lock) {
    if (!lock) return;
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(spinlock_t* lock) {
    uint64_t flags = irq_save_disable();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    irq_restore(flags);
}

void kernel_lock_enter(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_lock(&g_kernel_lock);
        return;
    }
    if (cpu->kernel_lock_depth++ == 0) {
        spin_lock(&g_kernel_lock);
    }
}

void kernel_lock_exit(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_unlock(&g_kernel_lock);
        return;
    }
    if (cpu->kernel_lock_depth == 0) return;
    cpu->kernel_lock_depth--;
    if (cpu->kernel_lock_depth == 0) {
        spin_unlock(&g_kernel_lock);
    }
}

int kernel_lock_held(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        return g_kernel_lock.locked != 0;
    }
    return cpu->kernel_lock_depth != 0;
}

void kernel_yield(void) {
    struct cpu_local* cpu = get_cpu_local();
    uint32_t depth = cpu ? cpu->kernel_lock_depth : 0;
    for (uint32_t i = 0; i < depth; i++) kernel_lock_exit();
    schedule();
    for (uint32_t i = 0; i < depth; i++) kernel_lock_enter();
}
