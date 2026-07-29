#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/time.h"
#include "smp.h"
#include "spinlock.h"
#include "task.h"

uint64_t g_hhdm_offset;

static spinlock_t g_riscv64_kernel_lock;
static uint8_t g_riscv64_console_buf[256];
static uint32_t g_riscv64_console_head;
static uint32_t g_riscv64_console_tail;
static struct task* g_riscv64_console_waiter;

void net_poll(void) {
    riscv64_console_poll_input();
}

uint64_t arch_time_now_ms(void) {
    /*
     * QEMU virt/OpenSBI exposes the platform timer at 10 MHz.
     * Bootstrap and smoke only need a monotonic millisecond clock.
     */
    return riscv64_read_time() / 10000ULL;
}

uint64_t arch_time_cpu_ms(uint32_t cpu_id) {
    (void)cpu_id;
    return arch_time_now_ms();
}

uint64_t irq_save_disable(void) {
    uint64_t flags = riscv64_read_sstatus();
    riscv64_write_sstatus(flags & ~RISCV64_SSTATUS_SIE);
    return flags;
}

void irq_restore(uint64_t flags) {
    uint64_t sstatus = riscv64_read_sstatus();
    sstatus &= ~RISCV64_SSTATUS_SIE;
    sstatus |= (flags & RISCV64_SSTATUS_SIE);
    riscv64_write_sstatus(sstatus);
}

void spinlock_init(spinlock_t* lock) {
    if (!lock) return;
    lock->locked = 0;
}

void spin_lock(spinlock_t* lock) {
    if (!lock) return;
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("nop");
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
        spin_lock(&g_riscv64_kernel_lock);
        return;
    }
    if (cpu->kernel_lock_depth++ == 0) {
        spin_lock(&g_riscv64_kernel_lock);
    }
}

void kernel_lock_exit(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_unlock(&g_riscv64_kernel_lock);
        return;
    }
    if (cpu->kernel_lock_depth == 0) return;
    cpu->kernel_lock_depth--;
    if (cpu->kernel_lock_depth == 0) {
        spin_unlock(&g_riscv64_kernel_lock);
    }
}

int kernel_lock_held(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        return g_riscv64_kernel_lock.locked != 0;
    }
    return cpu->kernel_lock_depth != 0;
}

void kernel_yield(void) {
    struct cpu_local* cpu = get_cpu_local();
    uint32_t depth = cpu ? cpu->kernel_lock_depth : 0;
    riscv64_console_poll_input();
    for (uint32_t i = 0; i < depth; i++) kernel_lock_exit();
    schedule();
    for (uint32_t i = 0; i < depth; i++) kernel_lock_enter();
}

void puts(const char* s) {
    riscv64_uart_puts(s);
}

void puthex(uint64_t value) {
    riscv64_uart_puthex64(value);
}

void riscv64_console_poll_input(void) {
    int ch = riscv64_uart_getchar_nonblock();
    while (ch >= 0) {
        uint32_t next_head = (g_riscv64_console_head + 1U) % (uint32_t)sizeof(g_riscv64_console_buf);
        if (ch == '\r') ch = '\n';
        if (next_head != g_riscv64_console_tail) {
            g_riscv64_console_buf[g_riscv64_console_head] = (uint8_t)ch;
            g_riscv64_console_head = next_head;
            if (g_riscv64_console_waiter && g_riscv64_console_waiter->state == TASK_SLEEPING) {
                task_wake(g_riscv64_console_waiter);
                g_riscv64_console_waiter = 0;
            }
        }
        ch = riscv64_uart_getchar_nonblock();
    }
}

int riscv64_console_read(char* buf, int count) {
    int read = 0;
    if (!buf || count <= 0) return 0;
    while (read < count) {
        if (g_riscv64_console_head == g_riscv64_console_tail) break;
        buf[read++] = (char)g_riscv64_console_buf[g_riscv64_console_tail];
        g_riscv64_console_tail = (g_riscv64_console_tail + 1U) % (uint32_t)sizeof(g_riscv64_console_buf);
    }
    return read;
}

void riscv64_console_set_waiter(struct task* t) {
    g_riscv64_console_waiter = t;
}

void riscv64_console_clear_waiter(struct task* t) {
    if (g_riscv64_console_waiter == t) g_riscv64_console_waiter = 0;
}

/* smp_get_cpu_info / smp_get_started_cpu_count / smp_send_resched_ipi は
 * kernel/riscv64/smp.c に実装がある */

int bottom_half_run(void) {
    return 0;
}

int net_needs_poll_fallback(void) {
    return 0;
}

void kernel_panic(const char* file, int line, const char* func, const char* expr) {
    puts("\r\n*** KERNEL PANIC ***\r\n");
    puts("expr: ");
    puts(expr ? expr : "(null)");
    puts("\r\nfunc: ");
    puts(func ? func : "(null)");
    puts("\r\nfile: ");
    puts(file ? file : "(null)");
    puts(":0x");
    puthex((uint64_t)(uint32_t)line);
    puts("\r\nHALTING...\r\n");
    for (;;) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

int64_t sys_write_serial(const char* buf, size_t count) {
    if (!buf) return -1;
    for (size_t i = 0; i < count; i++) {
        riscv64_uart_putchar(buf[i]);
    }
    return (int64_t)count;
}
