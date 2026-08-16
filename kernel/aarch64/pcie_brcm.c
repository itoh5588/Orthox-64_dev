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
}
