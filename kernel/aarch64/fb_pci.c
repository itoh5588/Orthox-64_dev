/*
 * PCI の表示装置 (bochs-display) からフレームバッファを取る。
 *
 * ---- なぜ要るか ------------------------------------------------------------
 *
 * **QEMU virt には mailbox が無い**ので、Raspberry Pi の経路
 * (kernel/aarch64/fb.c) では画面が取れない。一方 **USB キーボードは
 * virt でしか踏めない** (raspi4b は PCIe を持っていない)。
 *
 * つまり「画面 + キーボード」を同時に試せる機械が無い。**virt に画面を
 * 足すのが唯一の道。**これで DOOM をキーで操作できることを実機の前に
 * 確かめられる。
 *
 * ---- なぜ bochs-display か -------------------------------------------------
 *
 * **PCI の BAR0 がそのまま線形のフレームバッファ**になっている、いちばん
 * 単純な装置。BAR は既に自分で配れる (kernel/aarch64/pci.c) ので、
 * 追加で要るのは解像度の設定だけ。
 *
 * ramfb は fw_cfg の DMA 手順が要り、virtio-gpu は 2D コマンドの実装が要る。
 * **どちらも「画面を出す」ためだけには重い。**
 *
 * ---- 呼ぶ位置 --------------------------------------------------------------
 *
 * **MMU の後、pci_init の後。** ECAM は MMU を入れてからでないと読めない。
 * そのため fb.c (mailbox 版) と違い、**ページテーブルが組み上がった後に
 * 自分で写像を足す。**
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/fb.h"
#include "aarch64/vm.h"
#include "pci.h"
#include "vmm.h"

void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);
void arch_syscall_flush_tlb(void);

/* QEMU の bochs-display */
#define BOCHS_VENDOR   0x1234
#define BOCHS_DEVICE   0x1111

/* VBE dispi のレジスタ。**BAR2 の MMIO の 0x500 から 2 バイトずつ並ぶ** */
#define DISPI_MMIO_OFF   0x500
#define DISPI_ID         0
#define DISPI_XRES       1
#define DISPI_YRES       2
#define DISPI_BPP        3
#define DISPI_ENABLE     4
#define DISPI_VIRT_WIDTH 6
#define DISPI_VIRT_HEIGHT 7
#define DISPI_X_OFFSET   8
#define DISPI_Y_OFFSET   9

#define DISPI_ENABLED      0x01
#define DISPI_LFB_ENABLED  0x40

static uint64_t g_mmio;    /* BAR2 (物理) */

static volatile uint16_t* dispi(uint32_t index) {
    uint64_t pa = g_mmio + DISPI_MMIO_OFF + (uint64_t)index * 2ULL;
    return (volatile uint16_t*)(uintptr_t)aarch64_phys_to_virt(pa);
}

static void puthex_n(uint64_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    aarch64_uart_puts("0x");
    for (int i = digits - 1; i >= 0; i--) aarch64_uart_putchar(d[(v >> (i * 4)) & 0xf]);
}

static void put_dec(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) { aarch64_uart_putchar('0'); return; }
    while (v > 0 && i < (int)sizeof(buf)) { buf[i++] = (char)('0' + (int)(v % 10U)); v /= 10U; }
    while (i > 0) aarch64_uart_putchar(buf[--i]);
}

/* fb.c の中身を書き換える口。**画面の出所が 2 つある**ので、
 * どちらから来ても同じ struct を通して使えるようにする */
void aarch64_fb_set_info(uint64_t base, uint32_t size, uint32_t pitch,
                         uint32_t width, uint32_t height, uint32_t depth);

int aarch64_fb_init_pci(uint32_t want_w, uint32_t want_h) {
    struct pci_device_info dev;
    uint64_t fb_pa, mmio_pa, fb_size;

    if (want_w == 0) want_w = 1024;
    if (want_h == 0) want_h = 768;

    if (pci_find_device(&dev, BOCHS_VENDOR, BOCHS_DEVICE, -1, -1, -1) < 0) return -1;

    fb_pa   = pci_get_bar_mmio(&dev, 0);
    mmio_pa = pci_get_bar_mmio(&dev, 2);
    if (fb_pa == 0 || mmio_pa == 0) return -2;
    pci_enable_mmio_busmaster(&dev);
    g_mmio = mmio_pa;

    /* **BAR は MMIO 窓の中にあり、vm.c が窓ごと張ってある。**
     * 足りていなければここで落ちるので、黙って壊れることはない */

    /* **有効を落としてから解像度を書く。**表示中に変えると中途半端な
     * 状態が見える (仕様上も一度止めてから設定する) */
    *dispi(DISPI_ENABLE) = 0;
    *dispi(DISPI_XRES) = (uint16_t)want_w;
    *dispi(DISPI_YRES) = (uint16_t)want_h;
    *dispi(DISPI_BPP) = 32;
    *dispi(DISPI_VIRT_WIDTH) = (uint16_t)want_w;
    *dispi(DISPI_VIRT_HEIGHT) = (uint16_t)want_h;
    *dispi(DISPI_X_OFFSET) = 0;
    *dispi(DISPI_Y_OFFSET) = 0;
    *dispi(DISPI_ENABLE) = DISPI_ENABLED | DISPI_LFB_ENABLED;

    /* 読み返す。**書けたことと効いたことは別** */
    {
        uint32_t w = *dispi(DISPI_XRES);
        uint32_t h = *dispi(DISPI_YRES);
        uint32_t bpp = *dispi(DISPI_BPP);
        if (w == 0 || h == 0 || bpp != 32) return -3;
        want_w = w; want_h = h;
    }

    fb_size = (uint64_t)want_w * want_h * 4ULL;

    /* **フレームバッファを専用の VA に張る。**mailbox 版と同じ番地を使う
     * ので、fbcon も DOOM も経路を分けずに済む。
     * **MMU は既に入っている**ので、ページテーブルに後から足す形になる */
    {
        uint64_t sz = (fb_size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);
        arch_vm_map_range(arch_vm_kernel_address_space(), AARCH64_FB_VA_BASE,
                          fb_pa, sz, AARCH64_VM_FB_FLAGS);
        arch_syscall_flush_tlb();
    }

    aarch64_fb_set_info(fb_pa, (uint32_t)fb_size, want_w * 4U, want_w, want_h, 32);

    aarch64_uart_puts("  fb pci    : ");
    puthex_n(fb_pa, 8);
    aarch64_uart_puts(" ");
    put_dec(want_w);
    aarch64_uart_puts("x");
    put_dec(want_h);
    aarch64_uart_puts("x32 pitch ");
    put_dec(want_w * 4U);
    aarch64_uart_puts("  ok (bochs-display)\n");
    return 0;
}
