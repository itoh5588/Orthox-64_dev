#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/time.h"
#include "smp.h"
#include "spinlock.h"
#include "task.h"

uint64_t g_hhdm_offset;

static spinlock_t g_riscv64_kernel_lock;
/*
 * コンソール入力リングと UART の受信 FIFO を守るロック。
 *
 * BKL では守れない: riscv64_console_poll_input() は kernel_yield() から
 * (BKL を保持しているとは限らない) と、タイマー割り込みの net_poll() から
 * 呼ばれ、どの hart からも走る。UART の RBR は読むと文字が消えるので、
 * 2 つの hart が同時に読むと片方が取った文字がリングに入らないまま失われる
 * (SMP=4 で `x=42` が `x=2` になる形で観測した)。head/tail の更新も競合する。
 *
 * 静的変数なのでゼロ初期化 = spinlock_init 済みと同じ (g_riscv64_kernel_lock
 * と同じ扱い)。
 */
static spinlock_t g_riscv64_console_lock;
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

/*
 * カーネル実行中はタイマー割り込みが入らないので、期限切れ起床をここで自前に回す。
 *
 * riscv64 のトラップは sstatus.SIE を落とすため、カーネルに入ったあとは割り込みが
 * 一切来ない。一方ユーザーモード実行中は sstatus.SIE と無関係に S 割り込みが入る
 * (現在の特権モードが S より低ければ常に有効) ので、タイマープリエンプションだけは
 * 効いているように見えていた。
 *
 * この非対称性のせいで「カーネル内でブロック待ちループを回している間はタイマー
 * 割り込みが 1 度も来ない」状態になる。期限切れ起床は task_poll_sleep_wakeups() の
 * 走査でしか起きないので、例えば「子が nanosleep で寝て、親が wait4 で
 * kernel_yield() を回す」形だと誰も子を起こせずシステム全体が停止する
 * (走れるタスクがある以上 idle には落ちないため、idle 側の wfi 窓も開かない)。
 *
 * kernel_yield() で sstatus.SIE を一時的に開ける手も試したが、カーネルスタック上で
 * トラップがネストして current_task が壊れた (store page fault)。既に
 * riscv64_console_poll_input() を直接呼んでいるのと同じ流儀でポーリングする方が、
 * 現状の「カーネルは割り込みを取らない」設計と整合する。
 */
void kernel_yield(void) {
    struct cpu_local* cpu = get_cpu_local();
    uint32_t depth = cpu ? cpu->kernel_lock_depth : 0;
    riscv64_console_poll_input();
    (void)task_poll_sleep_wakeups();
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
    struct task* waiter = 0;
    uint64_t flags = spin_lock_irqsave(&g_riscv64_console_lock);
    int ch = riscv64_uart_getchar_nonblock();
    while (ch >= 0) {
        uint32_t next_head = (g_riscv64_console_head + 1U) % (uint32_t)sizeof(g_riscv64_console_buf);
        if (ch == '\r') ch = '\n';
        if (next_head != g_riscv64_console_tail) {
            g_riscv64_console_buf[g_riscv64_console_head] = (uint8_t)ch;
            g_riscv64_console_head = next_head;
            if (g_riscv64_console_waiter && g_riscv64_console_waiter->state == TASK_SLEEPING) {
                waiter = g_riscv64_console_waiter;
                g_riscv64_console_waiter = 0;
            }
        }
        ch = riscv64_uart_getchar_nonblock();
    }
    spin_unlock_irqrestore(&g_riscv64_console_lock, flags);
    /* task_wake() は g_task_lock を取って IPI も飛ばすので、コンソールロックの
     * 外で呼ぶ (ロック順序を作らない) */
    if (waiter) task_wake(waiter);
}

/* poll/ppoll 用。リングにデータが残っているかだけを見る */
int riscv64_console_has_input(void) {
    int has;
    uint64_t flags = spin_lock_irqsave(&g_riscv64_console_lock);
    has = g_riscv64_console_head != g_riscv64_console_tail;
    spin_unlock_irqrestore(&g_riscv64_console_lock, flags);
    return has;
}

int riscv64_console_read(char* buf, int count) {
    int read = 0;
    uint64_t flags;
    if (!buf || count <= 0) return 0;
    flags = spin_lock_irqsave(&g_riscv64_console_lock);
    while (read < count) {
        if (g_riscv64_console_head == g_riscv64_console_tail) break;
        buf[read++] = (char)g_riscv64_console_buf[g_riscv64_console_tail];
        g_riscv64_console_tail = (g_riscv64_console_tail + 1U) % (uint32_t)sizeof(g_riscv64_console_buf);
    }
    spin_unlock_irqrestore(&g_riscv64_console_lock, flags);
    return read;
}

void riscv64_console_set_waiter(struct task* t) {
    uint64_t flags = spin_lock_irqsave(&g_riscv64_console_lock);
    g_riscv64_console_waiter = t;
    spin_unlock_irqrestore(&g_riscv64_console_lock, flags);
}

void riscv64_console_clear_waiter(struct task* t) {
    uint64_t flags = spin_lock_irqsave(&g_riscv64_console_lock);
    if (g_riscv64_console_waiter == t) g_riscv64_console_waiter = 0;
    spin_unlock_irqrestore(&g_riscv64_console_lock, flags);
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
