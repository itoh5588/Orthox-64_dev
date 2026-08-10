/*
 * コンソール入力 (P3)。PL011 の受信割り込みでリングに溜め、
 * 共有 kernel/fs.c の kb_* から取り出す。
 *
 * M3c-2b までは kernel/aarch64/stubs.c が kb_read で -1 を返していた
 * (「入力が無い」ではなく「入力の口が無い」)。対話 ash には本物が要る。
 *
 * ---- 閉じなければならない競合 --------------------------------------------
 *
 * 共有 fs.c の待ち方はこうなっている:
 *
 *     while (read_bytes == 0) {
 *         read_bytes = kb_read(buf, count);
 *         if (read_bytes == 0) {
 *             task_mark_sleeping(current);
 *             kb_set_waiter(current);       <- ここまでに届いた文字は?
 *             kernel_yield();
 *             kb_clear_waiter(current);
 *         }
 *     }
 *
 * **kb_read が空を返してから kb_set_waiter に着くまでの間に文字が届くと、
 * 割り込み側は待ち手を見つけられない。** リングには入るが誰も起こさないので、
 * 次の 1 文字が来るまで固まる。riscv64 は read と wait を 1 つのロックの中で
 * まとめて閉じている (kernel/riscv64/runtime.c の riscv64_console_read_or_wait)。
 *
 * aarch64 は共有 fs.c を使うので API を 3 つに分けざるを得ない。そこで
 * **kb_set_waiter の中でリングを見直し、既にデータがあればその場で起こす。**
 * 取りこぼした文字があっても、登録の瞬間に自分で気づける。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "task.h"

int  aarch64_uart_getchar_nonblock(void);
void aarch64_uart_enable_rx_irq(void);
void aarch64_uart_clear_rx_irq(void);
void aarch64_gic_enable_irq(unsigned intid);

/* 待ち手は「いま読もうとしているタスク」1 本だけ。**複数を並べない。**
 * 共有 fs.c は 1 タスクずつ set/clear するので、ここで列を持つと
 * clear し損ねた古い待ち手が残る危険のほうが大きい */
static struct task* volatile g_console_waiter;

#define CONSOLE_BUF_SIZE 256
static volatile uint8_t g_console_buf[CONSOLE_BUF_SIZE];
static volatile uint32_t g_console_head;
static volatile uint32_t g_console_tail;

/* 割り込みを止めて臨界区間に入る。**CPU 1 本前提。**
 * SMP を入れるときはスピンロックに置き換えること (riscv64 は
 * spin_lock_irqsave を使っている) */
static uint64_t console_lock(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    return daif;
}

static void console_unlock(uint64_t daif) {
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static int console_ring_empty(void) {
    return g_console_head == g_console_tail;
}

/* 受信割り込みから呼ばれる。FIFO を空にしてリングへ移す */
void aarch64_console_rx_irq(void) {
    struct task* waiter;
    int pushed = 0;
    uint64_t flags = console_lock();
    int ch = aarch64_uart_getchar_nonblock();

    while (ch >= 0) {
        uint32_t next = (g_console_head + 1U) % CONSOLE_BUF_SIZE;
        /* 端末は改行を CR で送る。**ここで LF に直す** —
         * ash は '\n' で行を区切るので、CR のままだと入力が確定しない */
        if (ch == '\r') ch = '\n';
        if (next != g_console_tail) {
            g_console_buf[g_console_head] = (uint8_t)ch;
            g_console_head = next;
            pushed = 1;
        }
        /* リングが満杯なら捨てる。**待つ側は割り込み文脈なので詰まらせない** */
        ch = aarch64_uart_getchar_nonblock();
    }
    waiter = g_console_waiter;
    aarch64_uart_clear_rx_irq();
    console_unlock(flags);

    /* task_wake はロックの外で呼ぶ (ロック順序を作らない) */
    if (pushed && waiter && waiter->state == TASK_SLEEPING) {
        task_wake(waiter);
    }
}

/* ---- 共有 kernel/fs.c が呼ぶ入口 ---------------------------------------- */

/* データがあるだけ読む。無ければ 0。
 * **-1 を返してはいけない。** stubs.c 時代は「口が無い」ことを表すために
 * 負を返していたが、いまは本物があるので「まだ来ていない」= 0 が正しい。
 * 0 を返せば fs.c が待ち手に登録して寝る */
int kb_read(char* buf, int count) {
    int read = 0;
    uint64_t flags;
    if (!buf || count <= 0) return 0;
    flags = console_lock();
    while (read < count && !console_ring_empty()) {
        buf[read++] = (char)g_console_buf[g_console_tail];
        g_console_tail = (g_console_tail + 1U) % CONSOLE_BUF_SIZE;
    }
    console_unlock(flags);
    return read;
}

void kb_set_waiter(struct task* t) {
    int has;
    uint64_t flags = console_lock();
    g_console_waiter = t;
    /* **登録した瞬間にリングを見直す。** kb_read が空を返してから
     * ここへ来るまでに届いた文字は、割り込み側からは「待ち手なし」に
     * 見えていて誰も起こしていない。自分で気づいて起きる */
    has = !console_ring_empty();
    console_unlock(flags);

    if (has && t && t->state == TASK_SLEEPING) task_wake(t);
}

void kb_clear_waiter(struct task* t) {
    uint64_t flags = console_lock();
    if (g_console_waiter == t) g_console_waiter = 0;
    console_unlock(flags);
}

/* poll/ppoll 用。リングに残っているかだけを見る */
int arch_console_has_input(void) {
    int has;
    uint64_t flags = console_lock();
    has = !console_ring_empty();
    console_unlock(flags);
    return has;
}

int arch_console_set_waiter(struct task* t) {
    kb_set_waiter(t);
    return 0;
}

void arch_console_clear_waiter(struct task* t) {
    kb_clear_waiter(t);
}

/* PL011 の受信 INTID。DTB から取れていればそれを使う。
 * aarch64_timer_intid と同じ形 */
uint32_t aarch64_uart_intid(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (b->uart_intid) return b->uart_intid;
    return AARCH64_UART_SPI_DEFAULT + AARCH64_SPI_INTID_BASE;
}

/* 受信の口を開ける。**PL011 と GIC の両方が要る。**
 * どちらか片方だけだと、割り込みは上がらないのに「有効にしたつもり」に
 * なる (virtio-blk で同じ形を踏んでいる) */
void aarch64_console_input_init(void) {
    g_console_head = 0;
    g_console_tail = 0;
    g_console_waiter = 0;
    aarch64_uart_enable_rx_irq();
    aarch64_gic_enable_irq(aarch64_uart_intid());
}
