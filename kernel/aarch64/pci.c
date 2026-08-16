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

/* ブリッジ (header type 0x01) のレジスタ */
#define PCI_IO_BASE_LIMIT   0x1c
#define PCI_BUS_NUMBERS     0x18   /* primary(7:0) / secondary(15:8) / subordinate(23:16) */
#define PCI_MEM_BASE_LIMIT  0x20   /* base(15:0) / limit(31:16)。番地 >> 16 の上位 12 ビット */
#define PCI_PREF_BASE_LIMIT 0x24

#define PCI_HEADER_TYPE_BRIDGE 0x01

/* ブリッジの窓は **1MB 単位**。番地の [31:20] だけが効く */
#define PCI_BRIDGE_WIN_ALIGN 0x00100000ULL

/* xHCI = クラス 0x0c (Serial Bus) / サブクラス 0x03 (USB) / prog-if 0x30 */
#define PCI_CLASS_SERIAL_BUS  0x0c
#define PCI_SUBCLASS_USB      0x03
#define PCI_PROGIF_XHCI       0x30

/* **見つけたデバイスを覚えておく。**
 *
 * 以前は探すたびにバスを走査し直していたが、**ブリッジの先まで辿るように
 * なると走査が高くつく**うえ、辿り方を 2 か所に書くことになる。
 * 走査は 1 回にして、結果を表に残す */
#define PCI_MAX_DEVICES 32
static struct pci_device_info g_devs[PCI_MAX_DEVICES];
static uint32_t g_dev_count;

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

/* ---- ブリッジの奥まで辿る ---------------------------------------------
 *
 * **バス 0 だけでは足りない。** Raspberry Pi 4 の実機では USB (VL805) が
 * ルートポートの先、**バス 01** にいる (2026-08-16 実機の lspci で確認)。
 * QEMU virt でも -device pcie-root-port の先に置くと同じ形になる。
 *
 * ブリッジを見つけたら:
 *   1. バス番号を割り当てる (primary / secondary / subordinate)
 *   2. **subordinate をいったん 0xff にする。** 奥に何バスあるか分かる前に
 *      設定空間を読む必要があるため。走査後に本当の値へ縮める
 *   3. 奥を走査する (再帰)
 *   4. **奥に配った BAR を覆う窓をブリッジに設定する。**
 *      これを忘れるとブリッジが素通ししてくれず、番地が届かない
 *
 * 窓は 1MB 単位。番地の [31:20] だけが効く */
static uint8_t g_bus_max;

static void pci_scan_bus(uint8_t bus);

static void pci_setup_bridge(uint8_t b, uint8_t d, uint8_t f) {
    uint8_t secondary = (uint8_t)(g_bus_max + 1U);
    uint64_t win_start, win_end;

    g_bus_max = secondary;

    /* **奥を読む前にバス番号を入れる。** 入れないと奥の設定空間に届かない */
    cfg_w32(b, d, f, PCI_BUS_NUMBERS,
            (uint32_t)b | ((uint32_t)secondary << 8) | (0xffU << 16));

    /* **窓の起点は 1MB 境界に揃える。**ブリッジの窓はそれ未満を表せない */
    g_mmio_next = (g_mmio_next + PCI_BRIDGE_WIN_ALIGN - 1) & ~(PCI_BRIDGE_WIN_ALIGN - 1);
    win_start = g_mmio_next;

    pci_scan_bus(secondary);

    /* 奥に何も無くても窓は 1MB 確保しておく (base > limit にすると無効化に
     * なり、後から足せなくなる) */
    if (g_mmio_next <= win_start) g_mmio_next = win_start + PCI_BRIDGE_WIN_ALIGN;
    win_end = ((g_mmio_next + PCI_BRIDGE_WIN_ALIGN - 1) &
               ~(PCI_BRIDGE_WIN_ALIGN - 1)) - 1;
    g_mmio_next = win_end + 1;

    /* **実際に使ったバスまでに縮める。** 0xff のままだと、そのブリッジが
     * 全部のバスを自分のものだと主張し続ける */
    cfg_w32(b, d, f, PCI_BUS_NUMBERS,
            (uint32_t)b | ((uint32_t)secondary << 8) | ((uint32_t)g_bus_max << 16));

    /* メモリ窓。**上位 12 ビットだけが効く** (番地 >> 16 の上位) */
    cfg_w32(b, d, f, PCI_MEM_BASE_LIMIT,
            (uint32_t)(((win_start >> 16) & 0xfff0U) |
                       (((win_end >> 16) & 0xfff0U) << 16)));

    /* **I/O とプリフェッチは使わない。** base > limit で無効にする —
     * 中途半端に有効だと、そこへの読み書きが黙って吸われる */
    cfg_w32(b, d, f, PCI_IO_BASE_LIMIT, 0x000000f0U);
    cfg_w32(b, d, f, PCI_PREF_BASE_LIMIT, 0x0000fff0U);

    /* ブリッジ自身も MEM デコードと bus master を開ける */
    {
        uint16_t cmd = cfg_r16(b, d, f, PCI_COMMAND);
        cfg_w16(b, d, f, PCI_COMMAND,
                (uint16_t)(cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
    }
}

static void pci_scan_bus(uint8_t bus) {
    for (uint32_t d = 0; d < 32; d++) {
        for (uint32_t f = 0; f < 8; f++) {
            uint16_t vid = cfg_r16(bus, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
            uint8_t hdr;
            if (vid == 0xffffU || vid == 0) { if (f == 0) break; else continue; }
            hdr = cfg_r8(bus, (uint8_t)d, (uint8_t)f, PCI_HEADER_TYPE);

            if ((hdr & 0x7fU) == PCI_HEADER_TYPE_BRIDGE) {
                pci_setup_bridge(bus, (uint8_t)d, (uint8_t)f);
            } else {
                assign_bars(bus, (uint8_t)d, (uint8_t)f);
                if (g_dev_count < PCI_MAX_DEVICES) {
                    fill_info(&g_devs[g_dev_count++], bus, (uint8_t)d, (uint8_t)f);
                }
            }
            /* multi-function でなければ関数 0 だけ見ればよい */
            if (f == 0 && (hdr & 0x80U) == 0) break;
        }
    }
}

void pci_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    g_ready = 0;
    g_ecam = 0;
    g_dev_count = 0;
    g_bus_max = 0;
    if (!b || b->pcie_ecam_base == 0 || b->pcie_mmio_base == 0) return;

    g_ecam = b->pcie_ecam_base;
    g_mmio_next = b->pcie_mmio_base;
    g_mmio_end  = b->pcie_mmio_base + b->pcie_mmio_size;
    g_ready = 1;

    /* **走査と BAR の割り当てを分けない。** 分けると「見つけたが配って
     * いない」状態が生まれ、後で気づきにくい */
    pci_scan_bus(0);
}

int pci_find_device(struct pci_device_info* out, int vendor_id, int device_id,
                    int class_code, int subclass, int prog_if) {
    if (!g_ready || !out) return -1;
    /* **走査時に作った表から引く。** ブリッジの先も入っている */
    for (uint32_t i = 0; i < g_dev_count; i++) {
        const struct pci_device_info* p = &g_devs[i];
        if (vendor_id  >= 0 && p->vendor_id  != (uint16_t)vendor_id)  continue;
        if (device_id  >= 0 && p->device_id  != (uint16_t)device_id)  continue;
        if (class_code >= 0 && p->class_code != (uint8_t)class_code)  continue;
        if (subclass   >= 0 && p->subclass   != (uint8_t)subclass)    continue;
        if (prog_if    >= 0 && p->prog_if    != (uint8_t)prog_if)     continue;
        *out = *p;
        return 0;
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
    for (uint32_t i = 0; i < g_dev_count; i++) {
        const struct pci_device_info* p = &g_devs[i];
        aarch64_uart_puts("  pci ");
        puthex_n(p->bus, 2);
        aarch64_uart_puts(":");
        puthex_n(p->device, 2);
        aarch64_uart_puts(".");
        puthex_n(p->function, 1);
        aarch64_uart_puts("  vid ");
        puthex_n(p->vendor_id, 4);
        aarch64_uart_puts(" did ");
        puthex_n(p->device_id, 4);
        aarch64_uart_puts(" class ");
        puthex_n(((uint64_t)p->class_code << 16) |
                 ((uint64_t)p->subclass << 8) | p->prog_if, 6);
        aarch64_uart_puts(" bar0 ");
        puthex_n(pci_get_bar0_mmio(p), 8);
        aarch64_uart_puts("\n");
    }
    /* **ブリッジは表に入れていない** (デバイスではないので)。
     * 何段辿ったかはバス番号の最大で分かる */
    aarch64_uart_puts("  pci       : ");
    puthex_n(g_dev_count, 2);
    aarch64_uart_puts(" 台 (バス 0..");
    puthex_n(g_bus_max, 2);
    aarch64_uart_puts(")\n");
}

int aarch64_pci_ready(void) { return g_ready; }
