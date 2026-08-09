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
void aarch64_timer_on_tick(void) {
    g_ticks++;
    write_tval(g_interval);
    write_ctl(CNTP_CTL_ENABLE);
}
