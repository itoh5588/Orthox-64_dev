/*
 * Raspberry Pi 4 (BCM2711) の PCIe — **いまは読むだけの探針。**
 *
 * ---- なぜ ECAM と別に要るか ------------------------------------------------
 *
 * QEMU virt の pci-host-ecam-generic は「設定空間がそのままメモリに写って
 * いる」総称の作りで、kernel/aarch64/pci.c がそれを扱う。
 * **BCM2711 は違う。**設定空間は間接的にしか触れず、外向きの窓も
 * PCI 側と CPU 側で番地が違う (実機の DTB: PCI 0xc0000000 -> CPU 0x6_00000000)。
 *
 * この先に **VL805 (xHCI)** がぶら下がっていて、実機の USB はそこにある。
 *
 * ---- なぜ書き込まないか ----------------------------------------------------
 *
 * **BCM2711 の PCIe レジスタは公式資料の記述が薄い。**手元に確かな仕様が
 * 無い状態で初期化の手順を推測で書くと、当たっても外れても原因が分からない。
 *
 * **しかも Raspberry Pi のファームウェア (start4.elf) は起動時に PCIe を
 * 初期化し、VL805 のファームウェアも SPI EEPROM から入れる。**
 * つまり**私たちが起動した時点で既にリンクが上がっている可能性がある。**
 * そうであれば、ゼロから初期化する必要は無く、設定空間を読むだけで済む。
 *
 * **それを確かめるまで書き込まない。**まずレジスタ窓の中身を出して、
 * 何が見えているかを実機で見る。
 *
 * ---- QEMU では動かない ----------------------------------------------------
 *
 * **raspi4b は PCIe を持っていない** (QEMU が brcm,bcm2711-pcie を無効に
 * している)。**実機でしか通らない道** なので、ここは QEMU で検証できない。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/vm.h"

void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);

static void puthex_n(uint64_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    aarch64_uart_puts("0x");
    for (int i = digits - 1; i >= 0; i--) aarch64_uart_putchar(d[(v >> (i * 4)) & 0xf]);
}

static uint64_t g_base;

__attribute__((unused))
static uint32_t rd32(uint32_t off) {
    uint64_t pa = g_base + off;
    if (aarch64_vm_mmu_enabled()) return *(volatile uint32_t*)(uintptr_t)aarch64_phys_to_virt(pa);
    return *(volatile uint32_t*)(uintptr_t)pa;
}

/* **PCIe のルートポートは type 1 の設定ヘッダを持つ。**多くの作りでは
 * レジスタ窓の先頭がそれになっている。まずそこを読んで、
 * 「ベンダ ID が読めるか」を見る。0xffff なら誰も居ない */
#define RC_CFG_VENDOR   0x0000
#define RC_CFG_CLASS    0x0008
#define RC_CFG_BUSNUM   0x0018

int aarch64_pcie_brcm_probe(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (!b || b->pcie_brcm_base == 0) return -1;
    g_base = b->pcie_brcm_base;

#ifndef AARCH64_PCIE_BRCM_PROBE
    /* **既定では触らない。実機が起動しなくなる。**
     *
     * 2026-08-16 の実測 (Raspberry Pi 4 本体):
     *
     *   pcie probe: レジスタを読む (ここで止まったら窓が死んでいる)
     *   pcie raw  : 0x00000000 0x00000000 0x00000000     <- ここで停止
     *
     * **最初の 3 語は例外なく読めて全部 0、4 語目 (+0x0C) で固まった。**
     * ブロックにクロックか電源が来ていない。
     *
     * **「ファームウェアが既にリンクを上げているかもしれない」という
     * 見立ては外れた。**start4.elf は PCIe を立ち上げていない (少なくとも
     * ARM から見える形では)。第 2 段は本当にゼロから初期化が要る。
     *
     * 調べ直すときは
     *   make ... AARCH64_PCIE_BRCM_PROBE=1
     * で有効にする。**巻き戻せる状態で試すこと** */
    aarch64_uart_puts("  pcie brcm : ");
    puthex_n(b->pcie_brcm_base, 8);
    aarch64_uart_puts("  (dtb。触らない — 4 語目で固まる実測あり)\n");
    return -4;
#else

    aarch64_uart_puts("  pcie brcm : ");
    puthex_n(b->pcie_brcm_base, 8);
    aarch64_uart_puts(" size ");
    puthex_n(b->pcie_brcm_size, 8);
    aarch64_uart_puts("  (dtb)\n");
    aarch64_uart_puts("  pcie win  : PCI ");
    puthex_n(b->pcie_brcm_pci_base, 8);
    aarch64_uart_puts(" -> CPU ");
    puthex_n(b->pcie_brcm_cpu_base, 10);
    aarch64_uart_puts(" size ");
    puthex_n(b->pcie_brcm_win_size, 8);
    aarch64_uart_puts("\n");

    /* **ここから先で初めてレジスタを触る。**
     *
     * 実機で最悪なのは「無応答で固まる」こと。**触る直前に印を出して
     * おけば、止まった場所が 1 行で分かる。**この行が出て次が出なければ、
     * レジスタ窓が生きていない (電源かクロック) と確定できる */
    aarch64_uart_puts("  pcie probe: レジスタを読む (ここで止まったら窓が死んでいる)\n");

    /* **窓の先頭を生で出す。** 意味づけはしない — 何が見えているかを
     * 実機で確かめるのが目的。全部 0 なら電源が来ていない、全部 f なら
     * 番地が違う (どちらも見れば分かる) */
    aarch64_uart_puts("  pcie raw  :");
    for (uint32_t i = 0; i < 8; i++) {
        aarch64_uart_puts(" ");
        puthex_n(rd32(i * 4U), 8);
    }
    aarch64_uart_puts("\n");

    {
        uint32_t vid_did = rd32(RC_CFG_VENDOR);
        uint16_t vid = (uint16_t)(vid_did & 0xffffU);
        aarch64_uart_puts("  pcie rc   : vid ");
        puthex_n(vid, 4);
        aarch64_uart_puts(" did ");
        puthex_n(vid_did >> 16, 4);
        aarch64_uart_puts(" class ");
        puthex_n(rd32(RC_CFG_CLASS) >> 8, 6);
        aarch64_uart_puts(" bus ");
        puthex_n(rd32(RC_CFG_BUSNUM) & 0xffffffU, 6);
        if (vid == 0xffffU) {
            aarch64_uart_puts("  BAD (誰も応答しない)\n");
            return -2;
        }
        if (vid == 0) {
            aarch64_uart_puts("  BAD (全部 0。窓が生きていない)\n");
            return -3;
        }
        /* **14e4 なら Broadcom。**ファームウェアがルートポートを
         * 立ち上げている証拠になる */
        aarch64_uart_puts(vid == 0x14e4U ? "  ok (Broadcom のルートポートが応答した)\n"
                                         : "  ? (応答はあるが Broadcom ではない)\n");
    }
    return 0;
#endif
}

/* ==========================================================================
 * ルートコンプレックスの立ち上げ (第 2 段)
 *
 * **QEMU では一切検証できない。** raspi4b は PCIe を持っていないので、
 * ここは実機でしか通らない。既定で無効:
 *
 *     make ... AARCH64_PCIE_BRCM_INIT=1
 *
 * ---- 読まずに書く --------------------------------------------------------
 *
 * **ブロックが眠っている間の読み出しが危険。** 実測で、リセット状態の
 * まま読むと 3 語目までは 0 が返り 4 語目で固まった。読み書きを混ぜた
 * 「read-modify-write」は使わない。
 *
 * 代わりに**実測した「上がった状態の値」をそのまま書く。**
 * Raspberry Pi OS が立ち上げた後の値を採ってあるので、目標が分かっている
 * (Docs/pi4-pcie-notes.md §5c)。読むのは**リンク判定の 1 か所だけ**。
 *
 * ---- ビットの出所 --------------------------------------------------------
 *
 * Linux の pcie-brcmstb.c から**値だけ**を確認した (コードは持ち込んで
 * いない)。実測値と辻褄が合うことも確かめてある:
 *
 *   PCIE_STATUS の PORT|DL_ACTIVE|PHYLINKUP = 0x80|0x20|0x10 = 0xB0
 *   実測した「上がった状態」の +0x4068 = 0x000000B0   <- 一致
 * ========================================================================== */

#define RGR1_SW_INIT_1        0x9210
#define RGR1_PERST_MASK       0x00000001U
#define RGR1_INIT_GENERIC     0x00000002U

#define MISC_MISC_CTRL        0x4008
#define MISC_MEM_WIN0_LO      0x400c
#define MISC_PCIE_STATUS      0x4068
#define MISC_MEM_WIN0_LIMIT   0x4070
#define MISC_HARD_DEBUG       0x4204

/* PCIE_STATUS。**3 つ揃って初めてリンクが上がったと言える** */
#define STATUS_PORT           0x00000080U
#define STATUS_DL_ACTIVE      0x00000020U
#define STATUS_PHYLINKUP      0x00000010U
#define STATUS_LINK_UP        (STATUS_PORT | STATUS_DL_ACTIVE | STATUS_PHYLINKUP)

/* **実測値をそのまま書く。**個々のビットを組み立てるより、
 * 「動いている状態」を再現するほうが確実で、突き合わせもできる */
#define MISC_CTRL_UP_VALUE    0x88003480U   /* SCB_ACCESS_EN | CFG_READ_UR_MODE 等 */
#define HARD_DEBUG_UP_VALUE   0x00200000U   /* SerDes IDDQ (0x08000000) が落ちている */
#define MEM_WIN0_LO_VALUE     0xC0000000U   /* 外向き窓 = PCI 0xc0000000 */
#define MEM_WIN0_LIMIT_VALUE  0x3FF00000U

/* **既定ビルドでは使われない。**探針も立ち上げも opt-in なので */
__attribute__((unused))
static void wr32(uint32_t off, uint32_t v) {
    uint64_t pa = g_base + off;
    volatile uint32_t* p = aarch64_vm_mmu_enabled()
        ? (volatile uint32_t*)(uintptr_t)aarch64_phys_to_virt(pa)
        : (volatile uint32_t*)(uintptr_t)pa;
    *p = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* **時間待ちは CNTPCT_EL0 で測る。**空回しの回数だと CPU の速さで変わる。
 *
 * ★ **aarch64_timer_ticks() を使ってはいけない。**あれは
 * 「**タイマ割り込みが入った回数**」であって 54MHz のカウンタではない。
 * 取り違えて 200us のつもりで 10800 割り込み (≒ 100 秒以上) 待つ関数を
 * 書き、**実機が固まった。**「ハードウェアが書き込みで固まる」と誤診
 * しかけた (2026-08-16)。
 *
 * emmc2.c と runtime.c は最初から CNTPCT_EL0 を読んでいる。そちらに倣う */
static inline uint64_t pcie_cntpct(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}
static inline uint64_t pcie_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

__attribute__((unused))
static void pcie_delay_us(uint32_t us) {
    uint64_t f = pcie_cntfrq();
    uint64_t t0, want;
    if (f == 0) { for (volatile uint32_t i = 0; i < us * 100U; i++) { } return; }
    want = (f / 1000000ULL) * (uint64_t)us;
    t0 = pcie_cntpct();
    while (pcie_cntpct() - t0 < want) { __asm__ volatile("yield"); }
}

int aarch64_pcie_brcm_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (!b || b->pcie_brcm_base == 0) return -1;
    g_base = b->pcie_brcm_base;

#ifndef AARCH64_PCIE_BRCM_INIT
    (void)0;
    return -9;   /* 既定では何もしない */
#else
    /* **段ごとに印を出す。**実機で固まったとき、どこまで進んだかが
     * 1 行で分かる。QEMU で試せない以上、これが唯一の手がかりになる */
    aarch64_uart_puts("  pcie init : 1 リセットを立てる\n");
    wr32(RGR1_SW_INIT_1, RGR1_PERST_MASK | RGR1_INIT_GENERIC);
    pcie_delay_us(200);

    aarch64_uart_puts("  pcie init : 2 ブリッジのリセットを落とす (PERST は立てたまま)\n");
    wr32(RGR1_SW_INIT_1, RGR1_PERST_MASK);
    pcie_delay_us(200);

    aarch64_uart_puts("  pcie init : 3 SerDes の IDDQ を落とす\n");
    wr32(MISC_HARD_DEBUG, HARD_DEBUG_UP_VALUE);
    pcie_delay_us(200);

    aarch64_uart_puts("  pcie init : 4 MISC_CTRL と外向き窓\n");
    wr32(MISC_MISC_CTRL, MISC_CTRL_UP_VALUE);
    wr32(MISC_MEM_WIN0_LO, MEM_WIN0_LO_VALUE);
    wr32(MISC_MEM_WIN0_LIMIT, MEM_WIN0_LIMIT_VALUE);

    aarch64_uart_puts("  pcie init : 5 PERST を落とす\n");
    wr32(RGR1_SW_INIT_1, 0);
    pcie_delay_us(100000);   /* 100ms。リンク訓練に時間がかかる */

    /* **ここで初めて読む。**待ちには必ず上限を付ける */
    aarch64_uart_puts("  pcie init : 6 リンクを待つ\n");
    {
        uint32_t st = 0;
        for (int i = 0; i < 100; i++) {
            st = rd32(MISC_PCIE_STATUS);
            if ((st & STATUS_LINK_UP) == STATUS_LINK_UP) break;
            pcie_delay_us(10000);
        }
        aarch64_uart_puts("  pcie link : status ");
        puthex_n(st, 8);
        if ((st & STATUS_LINK_UP) == STATUS_LINK_UP) {
            aarch64_uart_puts("  ok (リンクが上がった)\n");
        } else {
            aarch64_uart_puts("  BAD (上がらない。期待は 0xb0 のビット)\n");
            return -2;
        }
    }

    /* ルートポートが名乗るか。**実測の 0x271114E4 と突き合わせる** */
    {
        uint32_t vid_did = rd32(RC_CFG_VENDOR);
        aarch64_uart_puts("  pcie rc   : ");
        puthex_n(vid_did, 8);
        aarch64_uart_puts(vid_did == 0x271114E4U ? "  ok (実測値と一致)\n"
                                                 : "  ? (実測は 0x271114e4)\n");
    }
    return 0;
#endif
}

/* ==========================================================================
 * 下流 (VL805) へ辿る
 *
 * **ECAM ではない。** 設定空間は索引とデータの 2 段:
 *
 *   EXT_CFG_INDEX (0x9000) に (bus << 20) | (devfn << 12) を書く
 *   EXT_CFG_DATA  (0x8000) + (off & 0xfff) を読み書きする
 *
 * **バス 0 (ルートポート自身) は直接。** 窓の先頭 (0x0000) がその設定
 * ヘッダで、索引を書く必要はない。実測で 0x271114E4 を確認済み。
 *
 * 出所は Linux の pcie-brcmstb.c の PCIE_ECAM_OFFSET (値だけ確認)。
 * ========================================================================== */

#define EXT_CFG_INDEX  0x9000
#define EXT_CFG_DATA   0x8000

static uint32_t brcm_cfg_r32(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t off) {
    if (bus == 0) {
        /* **ルートポートは直接。** devfn 0 以外は居ない */
        if (dev != 0 || fn != 0) return 0xffffffffU;
        return rd32(off & 0xfffU);
    }
    {
        uint32_t devfn = ((uint32_t)dev << 3) | fn;
        wr32(EXT_CFG_INDEX, ((uint32_t)bus << 20) | (devfn << 12));
        return rd32(EXT_CFG_DATA + (off & 0xfffU));
    }
}

static void brcm_cfg_w32(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t off, uint32_t v) {
    if (bus == 0) {
        if (dev != 0 || fn != 0) return;
        wr32(off & 0xfffU, v);
        return;
    }
    {
        uint32_t devfn = ((uint32_t)dev << 3) | fn;
        wr32(EXT_CFG_INDEX, ((uint32_t)bus << 20) | (devfn << 12));
        wr32(EXT_CFG_DATA + (off & 0xfffU), v);
    }
}

/* 見つけた xHCI。**CPU から見た BAR の番地**を覚える
 * (PCI 側とは違う。ranges で CPU 0x6_00000000 <-> PCI 0xc0000000) */
static uint64_t g_xhci_cpu_bar;
uint64_t aarch64_pcie_brcm_xhci_bar(void) { return g_xhci_cpu_bar; }

/* 下流を走査して xHCI を見つけ、BAR を配る。
 *
 * **BAR に書くのは PCI 側の番地、CPU が触るのは別の番地。**
 * ここを取り違えると「BAR は配れたのにレジスタが読めない」になる */
int aarch64_pcie_brcm_scan(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t pci_base, cpu_base;
    int found = 0;

    if (!b || g_base == 0) return -1;
    pci_base = b->pcie_brcm_pci_base;
    cpu_base = b->pcie_brcm_cpu_base;
    g_xhci_cpu_bar = 0;

    /* **ルートポートのバス番号を入れる。** 入れないと下流に届かない */
    brcm_cfg_w32(0, 0, 0, 0x18, 0x00010100U);   /* primary 0 / secondary 1 / subordinate 1 */

    for (uint32_t d = 0; d < 32 && !found; d++) {
        for (uint32_t f = 0; f < 8; f++) {
            uint32_t vid_did = brcm_cfg_r32(1, (uint8_t)d, (uint8_t)f, 0x00);
            uint32_t cls;
            if ((vid_did & 0xffffU) == 0xffffU || (vid_did & 0xffffU) == 0) {
                if (f == 0) break; else continue;
            }
            cls = brcm_cfg_r32(1, (uint8_t)d, (uint8_t)f, 0x08) >> 8;

            aarch64_uart_puts("  pcie dev  : 01:");
            puthex_n(d, 2);
            aarch64_uart_puts(".");
            puthex_n(f, 1);
            aarch64_uart_puts("  vid ");
            puthex_n(vid_did & 0xffffU, 4);
            aarch64_uart_puts(" did ");
            puthex_n(vid_did >> 16, 4);
            aarch64_uart_puts(" class ");
            puthex_n(cls, 6);
            aarch64_uart_puts("\n");

            /* xHCI (0x0c0330) を見つけたら BAR を配る */
            if (cls == 0x0c0330U) {
                uint32_t orig = brcm_cfg_r32(1, (uint8_t)d, (uint8_t)f, 0x10);
                uint32_t size_mask, size;

                brcm_cfg_w32(1, (uint8_t)d, (uint8_t)f, 0x10, 0xffffffffU);
                size_mask = brcm_cfg_r32(1, (uint8_t)d, (uint8_t)f, 0x10) & 0xfffffff0U;
                brcm_cfg_w32(1, (uint8_t)d, (uint8_t)f, 0x10, orig);
                size = (~size_mask) + 1U;

                /* **BAR には PCI 側の番地を書く** */
                brcm_cfg_w32(1, (uint8_t)d, (uint8_t)f, 0x10,
                             (uint32_t)pci_base | (orig & 0xfU));
                if ((orig & 0x6U) == 0x4U) {   /* 64bit BAR なら上位も */
                    brcm_cfg_w32(1, (uint8_t)d, (uint8_t)f, 0x14, 0);
                }
                /* MEM デコードと bus master を開ける */
                {
                    uint32_t cmd = brcm_cfg_r32(1, (uint8_t)d, (uint8_t)f, 0x04);
                    brcm_cfg_w32(1, (uint8_t)d, (uint8_t)f, 0x04, cmd | 0x6U);
                }
                /* **CPU から見た番地はこちら** */
                g_xhci_cpu_bar = cpu_base;

                aarch64_uart_puts("  pcie xhci : PCI ");
                puthex_n(pci_base, 8);
                aarch64_uart_puts(" -> CPU ");
                puthex_n(cpu_base, 10);
                aarch64_uart_puts(" size ");
                puthex_n(size, 8);
                aarch64_uart_puts("  ok\n");
                found = 1;
                break;
            }
            if (f == 0) {
                uint32_t hdr = brcm_cfg_r32(1, (uint8_t)d, 0, 0x0c);
                if (((hdr >> 16) & 0x80U) == 0) break;
            }
        }
    }
    if (!found) {
        aarch64_uart_puts("  pcie xhci : 見つからない\n");
        return -2;
    }
    return 0;
}
