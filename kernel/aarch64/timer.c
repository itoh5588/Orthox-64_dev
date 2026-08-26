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
    if (timer_cpu_index() == 0) {
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
    }

    aarch64_task_on_tick();   /* 切り替えの印を立てる (実際の切り替えは IRQ の出口) */
    write_tval(g_interval);
    write_ctl(CNTP_CTL_ENABLE);
}
