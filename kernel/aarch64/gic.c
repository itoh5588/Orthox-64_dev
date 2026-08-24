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

uint32_t aarch64_timer_intid(void);

/* **アドレスは直書きしない。** DTB の intc から reg[0] / reg[1] を取る
 * (M2b)。QEMU virt と Pi 4 では位置が違う */
#define GICD_CTLR_OFF        0x000
#define GICD_ISENABLER_OFF   0x100   /* 32 本ずつ 1 レジスタ */
#define GICD_IPRIORITYR_OFF  0x400   /* 4 本ずつ 1 レジスタ */
#define GICD_ITARGETSR_OFF   0x800   /* 1 本 1 バイト。宛先 CPU のビットマスク */
#define GICD_SGIR_OFF        0xF00   /* CPU 間割り込みの送信 (SMP の P-5) */

#define GICC_CTLR_OFF   0x000
#define GICC_PMR_OFF    0x004        /* 優先度マスク */
#define GICC_IAR_OFF    0x00C        /* 割り込み番号の取得 */
#define GICC_EOIR_OFF   0x010        /* 完了通知 */

static uint64_t g_gicd_base = AARCH64_QEMU_VIRT_GICD_BASE;
static uint64_t g_gicc_base = AARCH64_QEMU_VIRT_GICC_BASE;

#define GICD_CTLR        (g_gicd_base + GICD_CTLR_OFF)
#define GICD_ISENABLER   (g_gicd_base + GICD_ISENABLER_OFF)
#define GICD_IPRIORITYR  (g_gicd_base + GICD_IPRIORITYR_OFF)
#define GICD_ITARGETSR   (g_gicd_base + GICD_ITARGETSR_OFF)
#define GICC_CTLR        (g_gicc_base + GICC_CTLR_OFF)
#define GICC_PMR         (g_gicc_base + GICC_PMR_OFF)
#define GICC_IAR         (g_gicc_base + GICC_IAR_OFF)
#define GICC_EOIR        (g_gicc_base + GICC_EOIR_OFF)

static inline void w32(uint64_t a, uint32_t v) { *(volatile uint32_t*)a = v; }
static inline uint32_t r32(uint64_t a) { return *(volatile uint32_t*)a; }

/* MMU を入れて上位 VA へ移った後は、MMIO も VA で触る必要がある (M3b)。
 * **恒等マッピングを外す前に呼ぶこと。** 外した後だと、次に GIC を
 * 触った瞬間に落ちる */
void aarch64_gic_set_base(uint64_t gicd, uint64_t gicc) {
    if (gicd) g_gicd_base = gicd;
    if (gicc) g_gicc_base = gicc;
}

/* ---- 全体 (Distributor)。CPU 0 が 1 度だけ ------------------------------- */
void aarch64_gic_init_global(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (b->gicd_base) g_gicd_base = b->gicd_base;
    if (b->gicc_base) g_gicc_base = b->gicc_base;

    w32(GICD_CTLR, 1);
}

/* ---- CPU ごと (CPU Interface)。**全コアが自分で呼ぶ** (SMP の P-5) -------
 *
 * GICv2 では **CPU Interface と INTID 0-31 のレジスタが CPU ごとに
 * バンクされている**。同じ番地に書いても、書いた CPU のぶんしか効かない。
 * 副コアがこれを呼ばないと、そのコアは**割り込みを 1 本も受け取れない**。 */
void aarch64_gic_init_cpu(void) {
    /* 優先度マスクを最も緩く (0xF0 = すべて通す)。
     * ここを 0 のままにすると「有効にしたのに一度も上がらない」になる */
    w32(GICC_PMR, 0xF0);
    w32(GICC_CTLR, 1);

    /* **SGI (0-15) を開ける。** これが CPU 間割り込みの受け口で、
     * resched IPI に SGI 0 を使う。0-31 はバンクされているので
     * **コアごとに開ける必要がある** */
    for (unsigned i = 0; i < 16; i++) {
        volatile uint8_t* prio = (volatile uint8_t*)(GICD_IPRIORITYR + i);
        *prio = 0;   /* PMR(0xF0) より小さくないと通らない */
    }
    w32(GICD_ISENABLER, 0x0000ffffU);   /* INTID 0-15 */

    /* **PPI もバンクされている。** タイマ (INTID 30) は CPU 0 が
     * aarch64_gic_enable_irq(30) で開けているが、それは**CPU 0 のぶんだけ**。
     * 副コアがこれを開けないと、そのコアは**タイマ割り込みを 1 度も
     * 受け取らない** — プリエンプションも時間切れの起床も効かなくなる。
     *
     * 実測 (P-5): 開ける前は副コアの tick が 0 回で、IPI で起きたときしか
     * 動いていなかった */
    {
        unsigned t = aarch64_timer_intid();
        if (t >= 16 && t < 32) {
            volatile uint8_t* prio = (volatile uint8_t*)(GICD_IPRIORITYR + t);
            *prio = 0;
            w32(GICD_ISENABLER, 1U << t);
        }
    }
}

/* 既存の呼び出し (CPU 0 の起動路) はこのまま。中身が 2 つに割れただけ */
void aarch64_gic_init(void) {
    aarch64_gic_init_global();
    aarch64_gic_init_cpu();
}

/* ---- CPU 間割り込みを送る (SMP の P-5) -----------------------------------
 *
 * riscv64 の SBI send_ipi に当たる。GICv2 は GICD_SGIR に 1 回書くだけ:
 *
 *   [25:24] TargetListFilter  00 = CPUTargetList のとおりに送る
 *   [23:16] CPUTargetList     宛先 CPU のビットマスク
 *   [3:0]   SGIINTID          割り込み番号 (0-15)
 *
 * **書く前に dsb が要る。** 送る側は先に共有データ (resched_pending など)
 * を書いてから起こす。順序が入れ替わると、起きた側が古い値を見る */
void aarch64_gic_send_sgi(uint32_t cpu_mask, unsigned intid) {
    __asm__ volatile("dsb ishst" ::: "memory");
    w32(g_gicd_base + GICD_SGIR_OFF,
        ((cpu_mask & 0xffU) << 16) | (intid & 0xfU));
}

void aarch64_gic_enable_irq(unsigned intid) {
    /* 優先度を最高 (0) にしておく。PMR(0xF0) より小さくないと通らない */
    volatile uint8_t* prio = (volatile uint8_t*)(GICD_IPRIORITYR + intid);
    *prio = 0;

    /* **SPI は宛先 CPU を書かないとどこにも届かない。**
     *
     * GICD_ITARGETSR は 1 本 1 バイトで、宛先 CPU のビットマスクを持つ。
     * **実機の GIC-400 はリセット値が 0** なので、有効化だけしても
     * 「どの CPU にも配らない」設定のまま黙って落ちる。
     *
     * QEMU は CPU が 1 個のとき宛先を見ないので、**virt のスモークでは
     * 素通りしていた**。実機の Pi 4 は 4 コアなので効いてくる
     * (rpi4-osdev の enable_interrupt_controller も target[irq] = 1 を書く)。
     *
     * **0-31 (SGI / PPI) は書かない。** 読み取り専用で、CPU ごとに
     * 個別に配線されている。タイマ (INTID 30) が実機で動いていたのは
     * PPI だからで、SPI が届く証拠にはならなかった */
    if (intid >= 32) {
        volatile uint8_t* target = (volatile uint8_t*)(GICD_ITARGETSR + intid);
        *target = 1;   /* CPU 0 へ */
    }

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
