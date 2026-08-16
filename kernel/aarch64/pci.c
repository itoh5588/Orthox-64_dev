/*
 * aarch64 の PCI アクセス (ECAM)。
 *
 * ---- x86 と何が違うか ------------------------------------------------------
 *
 * kernel/pci.c は **x86 のポート I/O (0xCF8 / 0xCFC)** で設定空間を叩く。
 * aarch64 にポート I/O は無く、代わりに **ECAM** — 設定空間がメモリに
 * 写像されている。番地の作り方は決まっている:
 *
 *   ECAM の先頭 + (bus << 20) + (device << 15) + (function << 12) + offset
 *
 * 窓は DTB の pci-host-ecam-generic ノードの reg から取る (dtb.c)。
 *
 * ---- BAR は自分で配る ------------------------------------------------------
 *
 * **-kernel で直接起動すると BAR が未設定。** 普通は UEFI やファームウェアが
 * 配るが、こちらはそれを経由しない。読むと 0 が返るので、
 *
 *   1. 0xffffffff を書いて読み返し、立っているビットから大きさを知る
 *   2. DTB の 32bit MMIO 窓から切り出して書き戻す
 *
 * という手順で自分で配る。**これを忘れると BAR0 が 0 のままで、
 * xHCI のレジスタを番地 0 に読みに行って沈黙する。**
 *
 * ---- いまの範囲 ------------------------------------------------------------
 *
 * **バス 0 しか見ない。** QEMU virt は qemu-xhci をバス 0 に置く。
 * ブリッジの先まで辿るのは必要になってから。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/vm.h"
#include "pci.h"

void aarch64_uart_puts(const char* s);
void aarch64_uart_puthex64(uint64_t v);
void aarch64_uart_putchar(char c);

/* 短い 16 進。**16 桁で出すと 1 行が読めない** (emmc2.c と同じ理由) */
static void puthex_n(uint64_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    aarch64_uart_puts("0x");
    for (int i = digits - 1; i >= 0; i--) aarch64_uart_putchar(d[(v >> (i * 4)) & 0xf]);
}

#define PCI_VENDOR_ID     0x00
#define PCI_DEVICE_ID     0x02
#define PCI_COMMAND       0x04
#define PCI_STATUS        0x06
#define PCI_REVISION      0x08
#define PCI_PROG_IF       0x09
#define PCI_SUBCLASS      0x0a
#define PCI_CLASS         0x0b
#define PCI_HEADER_TYPE   0x0e
#define PCI_BAR0          0x10
#define PCI_IRQ_LINE      0x3c

#define PCI_COMMAND_IO      (1U << 0)
#define PCI_COMMAND_MEMORY  (1U << 1)
#define PCI_COMMAND_MASTER  (1U << 2)

/* BAR の下位ビット。**大きさを測るときはここを落とす** */
#define PCI_BAR_IO          (1U << 0)
#define PCI_BAR_TYPE_MASK   (3U << 1)
#define PCI_BAR_TYPE_64     (2U << 1)
#define PCI_BAR_ADDR_MASK   0xfffffff0U

/* xHCI = クラス 0x0c (Serial Bus) / サブクラス 0x03 (USB) / prog-if 0x30 */
#define PCI_CLASS_SERIAL_BUS  0x0c
#define PCI_SUBCLASS_USB      0x03
#define PCI_PROGIF_XHCI       0x30

static uint64_t g_ecam;          /* ECAM の先頭 (物理) */
static uint64_t g_mmio_next;     /* BAR を配る位置 (物理) */
static uint64_t g_mmio_end;
static int      g_ready;

/* 設定空間の番地。**MMU の前後で見え方が変わる** ので触るたびに変換する
 * (pmm の管理情報や fb と同じ理屈) */
static volatile uint8_t* cfg_ptr(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t off) {
    uint64_t pa = g_ecam + ((uint64_t)bus << 20) + ((uint64_t)dev << 15) +
                  ((uint64_t)fn << 12) + off;
    if (aarch64_vm_mmu_enabled()) return (volatile uint8_t*)(uintptr_t)aarch64_phys_to_virt(pa);
    return (volatile uint8_t*)(uintptr_t)pa;
}

static uint32_t cfg_r32(uint8_t b, uint8_t d, uint8_t f, uint32_t o) {
    return *(volatile uint32_t*)cfg_ptr(b, d, f, o & ~3U);
}
static void cfg_w32(uint8_t b, uint8_t d, uint8_t f, uint32_t o, uint32_t v) {
    *(volatile uint32_t*)cfg_ptr(b, d, f, o & ~3U) = v;
}
static uint16_t cfg_r16(uint8_t b, uint8_t d, uint8_t f, uint32_t o) {
    return (uint16_t)(cfg_r32(b, d, f, o) >> ((o & 2U) * 8U));
}
static uint8_t cfg_r8(uint8_t b, uint8_t d, uint8_t f, uint32_t o) {
    return (uint8_t)(cfg_r32(b, d, f, o) >> ((o & 3U) * 8U));
}
static void cfg_w16(uint8_t b, uint8_t d, uint8_t f, uint32_t o, uint16_t v) {
    uint32_t shift = (o & 2U) * 8U;
    uint32_t cur = cfg_r32(b, d, f, o);
    cur &= ~(0xffffU << shift);
    cur |= ((uint32_t)v) << shift;
    cfg_w32(b, d, f, o, cur);
}

/* ---- BAR の大きさを測って配る --------------------------------------------
 *
 * **測るあいだは MEM デコードを止める。** 0xffffffff を書いている最中に
 * 有効なままだと、他のデバイスと重なった窓が一瞬できる */
static void assign_bars(uint8_t b, uint8_t d, uint8_t f) {
    uint16_t cmd = cfg_r16(b, d, f, PCI_COMMAND);
    cfg_w16(b, d, f, PCI_COMMAND, (uint16_t)(cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO)));

    for (uint32_t i = 0; i < 6; i++) {
        uint32_t off = PCI_BAR0 + i * 4U;
        uint32_t orig = cfg_r32(b, d, f, off);
        uint32_t size_mask, size;
        int is64;

        if (orig & PCI_BAR_IO) continue;          /* I/O 空間は aarch64 に無い */
        is64 = ((orig & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64);

        cfg_w32(b, d, f, off, 0xffffffffU);
        size_mask = cfg_r32(b, d, f, off) & PCI_BAR_ADDR_MASK;
        cfg_w32(b, d, f, off, orig);

        if (size_mask == 0) {                     /* この BAR は実装されていない */
            if (is64) i++;
            continue;
        }
        size = (~size_mask) + 1U;

        /* **境界は大きさに揃える。** PCI の決まりで、BAR の番地は
         * その BAR の大きさの倍数でなければならない */
        {
            uint64_t base = (g_mmio_next + size - 1U) & ~((uint64_t)size - 1U);
            if (base + size > g_mmio_end) {
                aarch64_uart_puts("  pci: BAR を配る空きが尽きた\n");
                break;
            }
            cfg_w32(b, d, f, off, (uint32_t)base | (orig & 0xfU));
            if (is64) cfg_w32(b, d, f, off + 4U, 0);   /* 上位は 0 = 4GB 未満に置く */
            g_mmio_next = base + size;
        }
        if (is64) i++;
    }
    cfg_w16(b, d, f, PCI_COMMAND, cmd);
}

static void fill_info(struct pci_device_info* out, uint8_t b, uint8_t d, uint8_t f) {
    out->bus = b; out->device = d; out->function = f;
    out->vendor_id  = cfg_r16(b, d, f, PCI_VENDOR_ID);
    out->device_id  = cfg_r16(b, d, f, PCI_DEVICE_ID);
    out->class_code = cfg_r8(b, d, f, PCI_CLASS);
    out->subclass   = cfg_r8(b, d, f, PCI_SUBCLASS);
    out->prog_if    = cfg_r8(b, d, f, PCI_PROG_IF);
    out->header_type= cfg_r8(b, d, f, PCI_HEADER_TYPE);
    out->irq_line   = cfg_r8(b, d, f, PCI_IRQ_LINE);
}

void pci_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    g_ready = 0;
    g_ecam = 0;
    if (!b || b->pcie_ecam_base == 0 || b->pcie_mmio_base == 0) return;

    g_ecam = b->pcie_ecam_base;
    g_mmio_next = b->pcie_mmio_base;
    g_mmio_end  = b->pcie_mmio_base + b->pcie_mmio_size;
    g_ready = 1;

    /* **見つけた順に BAR を配る。** 走査と割り当てを分けない —
     * 分けると「見つけたが配っていない」状態が生まれ、後で気づきにくい */
    for (uint32_t d = 0; d < 32; d++) {
        for (uint32_t f = 0; f < 8; f++) {
            uint16_t vid = cfg_r16(0, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
            if (vid == 0xffffU || vid == 0) { if (f == 0) break; else continue; }
            assign_bars(0, (uint8_t)d, (uint8_t)f);
            /* multi-function でなければ関数 0 だけ見ればよい */
            if (f == 0 && (cfg_r8(0, (uint8_t)d, 0, PCI_HEADER_TYPE) & 0x80U) == 0) break;
        }
    }
}

int pci_find_device(struct pci_device_info* out, int vendor_id, int device_id,
                    int class_code, int subclass, int prog_if) {
    if (!g_ready || !out) return -1;
    for (uint32_t d = 0; d < 32; d++) {
        for (uint32_t f = 0; f < 8; f++) {
            uint16_t vid = cfg_r16(0, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
            if (vid == 0xffffU || vid == 0) { if (f == 0) break; else continue; }
            fill_info(out, 0, (uint8_t)d, (uint8_t)f);
            if (vendor_id  >= 0 && out->vendor_id  != (uint16_t)vendor_id)  goto next;
            if (device_id  >= 0 && out->device_id  != (uint16_t)device_id)  goto next;
            if (class_code >= 0 && out->class_code != (uint8_t)class_code)  goto next;
            if (subclass   >= 0 && out->subclass   != (uint8_t)subclass)    goto next;
            if (prog_if    >= 0 && out->prog_if    != (uint8_t)prog_if)     goto next;
            return 0;
        next:
            if (f == 0 && (out->header_type & 0x80U) == 0) break;
        }
    }
    return -1;
}

int pci_find_xhci(struct pci_device_info* out) {
    return pci_find_device(out, -1, -1, PCI_CLASS_SERIAL_BUS,
                           PCI_SUBCLASS_USB, PCI_PROGIF_XHCI);
}

uint64_t pci_get_bar_mmio(const struct pci_device_info* dev, uint8_t bar_index) {
    uint32_t off, lo;
    uint64_t addr;
    if (!g_ready || !dev || bar_index >= 6) return 0;
    off = PCI_BAR0 + (uint32_t)bar_index * 4U;
    lo = cfg_r32(dev->bus, dev->device, dev->function, off);
    if (lo & PCI_BAR_IO) return 0;
    addr = lo & PCI_BAR_ADDR_MASK;
    if ((lo & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        addr |= (uint64_t)cfg_r32(dev->bus, dev->device, dev->function, off + 4U) << 32;
    }
    return addr;
}

uint64_t pci_get_bar0_mmio(const struct pci_device_info* dev) {
    return pci_get_bar_mmio(dev, 0);
}

void pci_enable_mmio_busmaster(const struct pci_device_info* dev) {
    uint16_t cmd;
    if (!g_ready || !dev) return;
    cmd = cfg_r16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    cfg_w16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/* ---- 起動ログ ------------------------------------------------------------
 *
 * **何が刺さっているかを出す。** BAR を配ったかどうかは番地を見れば分かる
 * ので、「見つけたのに 0 のまま」を見逃さない */
void aarch64_pci_dump(void) {
    if (!g_ready) {
        aarch64_uart_puts("  pci       : 無し (ECAM が取れていない)\n");
        return;
    }
    for (uint32_t d = 0; d < 32; d++) {
        for (uint32_t f = 0; f < 8; f++) {
            struct pci_device_info info;
            uint16_t vid = cfg_r16(0, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
            if (vid == 0xffffU || vid == 0) { if (f == 0) break; else continue; }
            fill_info(&info, 0, (uint8_t)d, (uint8_t)f);
            aarch64_uart_puts("  pci 00:");
            puthex_n(d, 2);
            aarch64_uart_puts(".");
            puthex_n(f, 1);
            aarch64_uart_puts("  vid ");
            puthex_n(info.vendor_id, 4);
            aarch64_uart_puts(" did ");
            puthex_n(info.device_id, 4);
            aarch64_uart_puts(" class ");
            puthex_n(((uint64_t)info.class_code << 16) |
                     ((uint64_t)info.subclass << 8) | info.prog_if, 6);
            aarch64_uart_puts(" bar0 ");
            puthex_n(pci_get_bar0_mmio(&info), 8);
            aarch64_uart_puts("\n");
            if (f == 0 && (info.header_type & 0x80U) == 0) break;
        }
    }
}

int aarch64_pci_ready(void) { return g_ready; }
