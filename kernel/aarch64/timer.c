/*
 * generic timer。riscv64 が SBI 経由で次回時刻を設定していたところを、
 * AArch64 は専用のシステムレジスタで行う。
 *
 *   CNTFRQ_EL0     周波数 (Hz)。QEMU virt では 62.5MHz
 *   CNTPCT_EL0     現在のカウンタ値 (単調増加)
 *   CNTP_TVAL_EL0  「あと何カウントで割り込むか」。書くと減算が始まる
 *   CNTP_CTL_EL0   bit0 有効 / bit1 マスク / bit2 発火中
 *
 * CVAL (絶対時刻) ではなく TVAL (相対) を使う。tick ごとに間隔を書き直す
 * だけで済み、riscv64 の「現在時刻 + 間隔を書く」より 1 手少ない。
 *
 * 割り込み番号は DTB の timer ノードの interrupts から取る (M2b)。
 * QEMU virt では <1 14 0x104> = PPI 14 → INTID 30。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "task.h"

#define TICK_HZ 100     /* 10ms 刻み。x86 / riscv64 の SCHED_TICK_MS と揃える */

#define CNTP_CTL_ENABLE  (1U << 0)
#define CNTP_CTL_IMASK   (1U << 1)

static uint64_t g_interval;
static volatile uint64_t g_ticks;

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_tval(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_ctl(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

uint64_t aarch64_timer_freq(void) {
    return read_cntfrq();
}

uint64_t aarch64_timer_ticks(void) {
    return g_ticks;
}

/* 非セキュア物理タイマの INTID。GIC に有効化を頼むときに使う */
uint32_t aarch64_timer_intid(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (b->timer_intid) return b->timer_intid;
    return AARCH64_TIMER_PPI_DEFAULT + AARCH64_PPI_INTID_BASE;
}

void aarch64_timer_init(void) {
    uint64_t freq = read_cntfrq();
    /* 周波数が 0 なら間隔が 0 になり、割り込みが際限なく上がって固まる。
     * 0 のときは既定値に退く (QEMU virt では 62.5MHz が入っている) */
    if (freq == 0) freq = 62500000UL;
    g_interval = freq / TICK_HZ;
    if (g_interval == 0) g_interval = 1;

    g_ticks = 0;
    write_tval(g_interval);
    write_ctl(CNTP_CTL_ENABLE);   /* IMASK は立てない = 割り込みを通す */
}

/* タイマ割り込みが上がったときに呼ぶ。次回ぶんを仕込み直す */
void aarch64_task_on_tick(void);

void aarch64_kbd_tick(void);

/* **鳴り終わったブロックを無音に戻す (D-3)。**PWM の FIFO を DMA が
 * 円環で埋め続けるので、戻さないと一周して同じ音がもう一度鳴る。
 * 1 ブロック 32ms に対しここは 10ms ごとなので取りこぼさない。
 * **音を持たない機械では何もしない** */
void sound_tick(void);

/* この CPU の番号。**記帳ではなく mpidr をそのまま読む** —
 * cpu_local が未設置の区間でも成立する (smp.c:98 と同じ形) */
static inline uint32_t timer_cpu_index(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xffULL);
}

/* ---- CPU ごとの稼働を数える (M-4 の切り分け) -----------------------------
 *
 * **「4 コアにしたのに縮まない」の原因を絞るための計器。**
 *
 * 実測 (2026-08-26): 実機でセルフホストのビルドを make -j4 で回したら
 * 50 分 9 秒。1 コアだった 8/23 の 52 分とほぼ同じで、**4 コアにした
 * 効果が出なかった。** ログからは「4 本並列で 50 分」と「逐次で 50 分」
 * が区別できない — 起動行に時刻が無いので、どちらでも同じに見える。
 *
 * **区別するには「各 CPU が実際に走っていた割合」を測るしかない。**
 *
 *   どのコアも高い    -> CPU は使い切っている。遅いのは別の所
 *                        (SD の I/O が本命。emmc2 は PIO のみで DMA 無し)
 *   cpu0 だけ高い     -> タスクが分散していない。スケジューラの問題
 *   どのコアも低い    -> 全員が何かを待っている
 *
 * タイマは PPI で**全 CPU に上がる**ので、自分の分は自分で数えられる。
 * tick ごとに加算 2 回だけなので、常時入れておいても割に合う。
 *
 * idle タスクを走らせていた tick は「暇」、それ以外は「働いた」。 */
static volatile uint64_t g_cpu_tick_all[AARCH64_MAX_CPUS];
static volatile uint64_t g_cpu_tick_busy[AARCH64_MAX_CPUS];
static volatile uint32_t g_cpu_runq_peak[AARCH64_MAX_CPUS];

static void cpu_stats_count(uint32_t cpu) {
    struct cpu_local* c;
    if (cpu >= AARCH64_MAX_CPUS) return;
    g_cpu_tick_all[cpu]++;
    /* **cpu_local がまだ無い時期にも tick は来る** (副コアが数に入る前)。
     * その間は「暇」として数える — 実際まだ何もしていない */
    c = get_cpu_local();
    if (!c) return;
    if (c->current_task && c->current_task != c->idle_task) {
        g_cpu_tick_busy[cpu]++;
    }
    if (c->runq_count > g_cpu_runq_peak[cpu]) g_cpu_runq_peak[cpu] = c->runq_count;
}

/* **区間ごとの割合を出す。** 累積だと平均に均されて、ビルドの山が
 * 見えなくなる。前回からの差分で出す。
 *
 * 呼ぶのは CPU 0 だけ。**待ち行列の頂点は出したら 0 に戻す** —
 * 「この 60 秒で最大いくつ積まれたか」が見たいので */
static void cpu_stats_report(void) {
    static uint64_t prev_all[AARCH64_MAX_CPUS];
    static uint64_t prev_busy[AARCH64_MAX_CPUS];
    static uint64_t prev_wait[AARCH64_MAX_CPUS][2];
    static uint64_t prev_pct;          /* 前回の CNTPCT。区間の長さに使う */
    uint64_t now, span;
    uint32_t i;
    uint32_t n = aarch64_boot_info()->cpu_count;
    if (n > AARCH64_MAX_CPUS) n = AARCH64_MAX_CPUS;

    /* **区間の長さは tick 数ではなく CNTPCT で測る。** 待ちの割合を
     * 出すには「実時間で何割」が要る。tick は 10ms 刻みで粗い */
    now  = aarch64_wait_now();
    span = now - prev_pct;
    prev_pct = now;

    aarch64_console_begin();
    aarch64_uart_puts("[cpu] 60s");
    for (i = 0; i < n; i++) {
        uint64_t all  = g_cpu_tick_all[i]  - prev_all[i];
        uint64_t busy = g_cpu_tick_busy[i] - prev_busy[i];
        uint64_t wsp  = aarch64_wait_get(i, 0) - prev_wait[i][0];   /* spinlock */
        uint64_t wsd  = aarch64_wait_get(i, 1) - prev_wait[i][1];   /* SD */
        prev_all[i]  = g_cpu_tick_all[i];
        prev_busy[i] = g_cpu_tick_busy[i];
        prev_wait[i][0] = aarch64_wait_get(i, 0);
        prev_wait[i][1] = aarch64_wait_get(i, 1);

        aarch64_uart_puts("  cpu");
        aarch64_uart_putdec64(i);
        aarch64_uart_puts(" ");
        /* tick が 0 の CPU は「上がっていない」。割り算で落ちないように */
        aarch64_uart_putdec64(all ? (busy * 100ULL) / all : 0ULL);
        /* **待ちの内訳。** busy のうち何割が待ちかがここで分かれる。
         * lk = spinlock を取るまで / sd = SD の応答待ち */
        aarch64_uart_puts("%(lk");
        aarch64_uart_putdec64(span ? (wsp * 100ULL) / span : 0ULL);
        aarch64_uart_puts(" sd");
        aarch64_uart_putdec64(span ? (wsd * 100ULL) / span : 0ULL);
        aarch64_uart_puts(" rq");
        aarch64_uart_putdec64(g_cpu_runq_peak[i]);
        aarch64_uart_puts(")");
        g_cpu_runq_peak[i] = 0;
    }
    aarch64_uart_puts("\n");
    aarch64_console_end();

    /* **SD への入出力を回数で出す (P-1)。**[cpu] の行と同じ 60 秒の区間で
     * 並べて読めるように、続けて出す。**量では説明がつかないと分かって
     * いる**ので、見たいのは回数と 1 回あたりの待ち (日報2026-08-29 §14) */
    aarch64_emmc2_io_report();
}

void aarch64_timer_on_tick(void) {
    /* **周期のポーリングは CPU 0 だけが行う (D-5)。**
     *
     * タイマは PPI なので 4 コア全部に上がる (gic.c:100)。ここを
     * 全コアで回すと、どちらもグローバルに 1 組しか無い状態を
     * 書くので壊れる — kbd は xHCI のイベントリングを取り合い
     * (usb.c:2611)、sound は g_clean_idx を同時に進める。
     *
     * **ロックで包むのではなく、叩く者を 1 人に戻す。** どちらも
     * 「定期的に 1 回」が意味で、四重に叩く意味が無い。ロックだと
     * kbd_tick の USB 転送 (ms 単位) を**割り込み文脈のまま 3 コアが
     * 待つ**ことになる。sched.c:133 の task_poll_sleep_wakeups と同じ形。
     *
     * **周期も直る。** 4 コアで回していた間は「10ms に 1 回」の
     * つもりで 4 倍叩いていた。
     *
     * **これで消えるのは tick 同士だけ。** tick と syscall (別のコアで
     * 走るユーザプロセス) の競合は残るので、そちらは kbd.c / sound.c
     * 側でロックを取る */
    uint32_t me = timer_cpu_index();

    /* **数えるのは全 CPU。** 出すのは CPU 0 だけ (下) */
    cpu_stats_count(me);

    if (me == 0) {
        /* **USB キーボードを拾う。**割り込みを繋いでいないので、ここが唯一の
         * 定期的な機会。文字が来ていればコンソールのリングへ流し、寝ている
         * シェルを起こす (kernel/aarch64/kbd.c)。
         * **キーボードが無い機械では即座に戻る** */
        aarch64_kbd_tick();

        /* 音の後片付け (D-3)。**鳴っていなければ即座に戻る** */
        sound_tick();

        /* **数えるのも CPU 0 だけ (D-5)。**
         *
         * これは「割り込みが入った回数」で、time ではなく**間隔**の
         * 物差しに使われている (virtio_blk_mmio.c:43 の
         * 「10ms x 300 = 3 秒」)。4 コアで数えると 4 倍の速さで進み、
         * **3 秒のつもりが 0.75 秒で時間切れになる。**
         * 非 atomic な ++ を 4 コアで叩いて数を落とす問題も同時に消える。
         *
         * 時刻 (arch_time_now_ms) は CNTPCT_EL0 から出しているので
         * ここには依存しない (runtime.c:44) */
        g_ticks++;

        /* 60 秒ごとに 1 行。100Hz なので 6000 tick */
        if ((g_ticks % 6000ULL) == 0ULL) cpu_stats_report();
    }

    aarch64_task_on_tick();   /* 切り替えの印を立てる (実際の切り替えは IRQ の出口) */
    write_tval(g_interval);
    write_ctl(CNTP_CTL_ENABLE);
}
