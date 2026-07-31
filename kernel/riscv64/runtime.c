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
 * BKL では守れない: riscv64_console_poll_input() は UART 受信割り込みから
 * 呼ばれ、リングの読み出し側 (riscv64_console_read_or_wait) は syscall 文脈で
 * 走る。UART の RBR は読むと文字が消えるので、取り合うと片方が取った文字が
 * リングに入らないまま失われる (SMP=4 で `x=42` が `x=2` になる形で観測した)。
 * head/tail の更新も競合する。
 *
 * 静的変数なのでゼロ初期化 = spinlock_init 済みと同じ (g_riscv64_kernel_lock
 * と同じ扱い)。
 */
static spinlock_t g_riscv64_console_lock;
static uint8_t g_riscv64_console_buf[256];
static uint32_t g_riscv64_console_head;
static uint32_t g_riscv64_console_tail;
static struct task* g_riscv64_console_waiter;

/* riscv64 にネットワークスタックは無い。コンソール入力もタイマー経由の
 * ポーリングをやめ、UART 受信割り込み (PLIC) だけで拾うようにしたので何もしない。
 * ここで保険を残すと、割り込みが死んでも 10Hz で動いてしまい気付けない */
void net_poll(void) {
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

/*
 * kernel_lock_depth は per-CPU だが read-modify-write なので、同一 CPU に
 * 割り込みが入ると壊れる (他 CPU との競合ではなく自分の割り込みが相手)。
 * カーネル実行中も割り込みを取るようになったので割り込みを閉じて更新する。
 *
 * 「spinlock の取得」と「depth を 1 にする」は割り込みに対して不可分でなければ
 * ならない。間で割り込まれると、ハンドラが depth==0 を見て spin_lock に入り、
 * 自分が握っているロックを自分の割り込みの中で待つデッドロックになる。
 * そのため取得中は割り込みを閉じたままにする (待たされるのは競合した CPU 側
 * だけで、単核では競合しない)。
 */
void kernel_lock_enter(void) {
    uint64_t flags = irq_save_disable();
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_lock(&g_riscv64_kernel_lock);
        irq_restore(flags);
        return;
    }
    if (cpu->kernel_lock_depth == 0) {
        spin_lock(&g_riscv64_kernel_lock);
    }
    cpu->kernel_lock_depth++;
    irq_restore(flags);
}

void kernel_lock_exit(void) {
    uint64_t flags = irq_save_disable();
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        spin_unlock(&g_riscv64_kernel_lock);
        irq_restore(flags);
        return;
    }
    if (cpu->kernel_lock_depth != 0) {
        cpu->kernel_lock_depth--;
        if (cpu->kernel_lock_depth == 0) {
            spin_unlock(&g_riscv64_kernel_lock);
        }
    }
    irq_restore(flags);
}

int kernel_lock_held(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) {
        return g_riscv64_kernel_lock.locked != 0;
    }
    return cpu->kernel_lock_depth != 0;
}

/*
 * ここではもう何もポーリングしない。
 *
 * - 期限切れ起床はタイマー割り込み (task_on_timer_tick)
 * - コンソール入力は UART 受信割り込み (PLIC 経由、riscv64_console_poll_input)
 *
 * どちらもカーネル実行中に届く (riscv64_interrupts_enable)。ここで保険として
 * 二重に回すと、割り込みが死んだときに気付けなくなる。
 */
void kernel_yield(void) {
    struct cpu_local* cpu = get_cpu_local();
    uint32_t depth = cpu ? cpu->kernel_lock_depth : 0;
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

/*
 * データがあれば読み、無ければ「待ち手」に登録して SLEEPING にする。
 * 戻り値 >0 = 読めた、0 = 寝る準備ができた (呼び出し側は kernel_yield すること)。
 *
 * 「空を確認する」と「寝る」の間を割り込みに対して不可分にする必要がある。
 * 分かれていると、その隙に届いた文字は UART 割り込みハンドラがリングへ入れる
 * ものの、まだ waiter が登録されていないので誰も起こしてくれず、次の 1 文字が
 * 来るまで固まる。コンソールロック (irqsave) の中で両方やることで閉じる。
 *
 * ロック順序は console -> task。task_wake() 側は必ずコンソールロックの外で
 * 呼んでいるので逆順にはならない。
 */
int riscv64_console_read_or_wait(char* buf, int count, struct task* self) {
    int read = 0;
    uint64_t flags;
    if (!buf || count <= 0) return 0;
    flags = spin_lock_irqsave(&g_riscv64_console_lock);
    while (read < count) {
        if (g_riscv64_console_head == g_riscv64_console_tail) break;
        buf[read++] = (char)g_riscv64_console_buf[g_riscv64_console_tail];
        g_riscv64_console_tail = (g_riscv64_console_tail + 1U) % (uint32_t)sizeof(g_riscv64_console_buf);
    }
    if (read == 0 && self) {
        g_riscv64_console_waiter = self;
        task_mark_sleeping(self);
    }
    spin_unlock_irqrestore(&g_riscv64_console_lock, flags);
    return read;
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
