/*
 * GICv2 (Generic Interrupt Controller)。riscv64 の PLIC に相当する。
 *
 * アドレスは DTB の intc から取る (M2b)。QEMU virt の実測値:
 *   intc@8000000  reg[0] = 0x08000000 size 0x10000  Distributor
 *                 reg[1] = 0x08010000 size 0x10000  CPU Interface
 *   compatible = arm,cortex-a15-gic (GICv2)
 *
 * **reg[1] を落とすと CPU Interface が無いまま進む。** Pi 4 は GIC-400 で
 * 名乗り方もアドレスも違うので、直書きでは動かない。
 *
 * 使い方は PLIC とほぼ同じ:
 *   1. Distributor で割り込みを有効化する
 *   2. 上がってきたら CPU Interface から番号を読む (PLIC の claim)
 *   3. 処理が終わったら CPU Interface に書き戻す (PLIC の complete)
 *
 * 割り込み番号の区分:
 *   0-15   SGI  CPU 間割り込み (riscv64 の IPI)
 *   16-31  PPI  CPU ごとに個別。**タイマは 30** (DTB の timer interrupts が
 *               <1 14 ...> = PPI 番号 14、PPI の INTID は +16 なので 30)
 *   32-    SPI  共有。UART や virtio
 */
#include <stdint.h>
#include "aarch64/boot.h"

/* **アドレスは直書きしない。** DTB の intc から reg[0] / reg[1] を取る
 * (M2b)。QEMU virt と Pi 4 では位置が違う */
#define GICD_CTLR_OFF        0x000
#define GICD_ISENABLER_OFF   0x100   /* 32 本ずつ 1 レジスタ */
#define GICD_IPRIORITYR_OFF  0x400   /* 4 本ずつ 1 レジスタ */

#define GICC_CTLR_OFF   0x000
#define GICC_PMR_OFF    0x004        /* 優先度マスク */
#define GICC_IAR_OFF    0x00C        /* 割り込み番号の取得 */
#define GICC_EOIR_OFF   0x010        /* 完了通知 */

static uint64_t g_gicd_base = AARCH64_QEMU_VIRT_GICD_BASE;
static uint64_t g_gicc_base = AARCH64_QEMU_VIRT_GICC_BASE;

#define GICD_CTLR        (g_gicd_base + GICD_CTLR_OFF)
#define GICD_ISENABLER   (g_gicd_base + GICD_ISENABLER_OFF)
#define GICD_IPRIORITYR  (g_gicd_base + GICD_IPRIORITYR_OFF)
#define GICC_CTLR        (g_gicc_base + GICC_CTLR_OFF)
#define GICC_PMR         (g_gicc_base + GICC_PMR_OFF)
#define GICC_IAR         (g_gicc_base + GICC_IAR_OFF)
#define GICC_EOIR        (g_gicc_base + GICC_EOIR_OFF)

static inline void w32(uint64_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static inline uint32_t r32(uint64_t a) { return *(volatile uint32_t*)a; }

void aarch64_gic_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (b->gicd_base) g_gicd_base = b->gicd_base;
    if (b->gicc_base) g_gicc_base = b->gicc_base;

    /* CPU Interface: 優先度マスクを最も緩く (0xF0 = すべて通す)。
     * ここを 0 のままにすると「有効にしたのに一度も上がらない」になる */
    w32(GICC_PMR, 0xF0);
    w32(GICC_CTLR, 1);

    /* Distributor を有効化 */
    w32(GICD_CTLR, 1);
}

void aarch64_gic_enable_irq(unsigned intid) {
    /* 優先度を最高 (0) にしておく。PMR(0xF0) より小さくないと通らない */
    volatile uint8_t* prio = (volatile uint8_t*)(GICD_IPRIORITYR + intid);
    *prio = 0;

    w32(GICD_ISENABLER + (intid / 32) * 4, 1U << (intid % 32));
}

/* 上がっている割り込みの番号を取る (PLIC の claim)。
 * 戻り値は EOI にそのまま渡す必要があるので、生の IAR を返す */
uint32_t aarch64_gic_claim(void) {
    return r32(GICC_IAR);
}

void aarch64_gic_complete(uint32_t iar) {
    w32(GICC_EOIR, iar);
}
