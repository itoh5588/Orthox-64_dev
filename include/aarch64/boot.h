#ifndef ORTHOX_ARCH_AARCH64_BOOT_H
#define ORTHOX_ARCH_AARCH64_BOOT_H

#include <stdint.h>

/* DTB が読めなかったときに退く値。**QEMU virt の実測値** であって、
 * Raspberry Pi 4 では全部違う (段取り 3 で効いてくる)。
 *
 *   Pi 4 の PL011 は 0xFE201000、GIC-400 は 0xFF841000 付近。
 *   **記憶で書かず、実機の DTB か公式資料で確認すること** (日報2026-08-09 E)
 */
/* RAM は**もう既定値に退かない** (下の FALLBACK を見ること)。
 * 実測値として残してあるだけで、コードからは参照していない */
#define AARCH64_QEMU_VIRT_RAM_BASE      0x40000000ULL
#define AARCH64_QEMU_VIRT_RAM_SIZE      0x20000000ULL   /* 512MB */
#define AARCH64_QEMU_VIRT_UART0_BASE    0x09000000ULL
#define AARCH64_QEMU_VIRT_GICD_BASE     0x08000000ULL
#define AARCH64_QEMU_VIRT_GICC_BASE     0x08010000ULL
#define AARCH64_QEMU_VIRT_GIC_SIZE      0x00010000ULL
#define AARCH64_QEMU_VIRT_VIRTIO_BASE   0x0a000000ULL
#define AARCH64_QEMU_VIRT_VIRTIO_STRIDE 0x00000200ULL

/* ---- RAM が DTB から取れなかったときの退き先 -----------------------------
 *
 * **機械名を直書きせず、カーネル自身のロード先から導く。**
 * ファームウェアはカーネルを RAM の中に置くので、ロード先を含む切りのいい
 * 境界まで下ろせば RAM の基点に当たる:
 *
 *   QEMU virt          0x40200000 -> 0x40000000   (従来の直書きと同じ値)
 *   Raspberry Pi 3/4   0x00080000 -> 0x00000000
 *
 * **Pi 4 で virt の 0x40000000 に退くと、存在しない RAM を張ったうえに
 * virt の virtio (0x0a000000) と重なってテーブル構築が失敗する** (実測:
 * block/table conflict at 0xffffff800a000000)。
 *
 * 大きさは**確実に存在するぶんだけ**にする。Pi 4 は最小構成でも 1GB、
 * QEMU virt のスモークは 512MB で回しているので 512MB なら両方で安全。
 * 実機では DTB をファームウェアが書き換えるので、ここへ退くのは
 * **配布 DTB をそのまま食わせた QEMU の場合**が主。 */
#define AARCH64_FALLBACK_RAM_ALIGN      0x40000000ULL   /* 1GB */
#define AARCH64_FALLBACK_RAM_SIZE       0x20000000ULL   /* 512MB */

/* ---- 早期 UART の既定値 --------------------------------------------------
 *
 * **DTB を読む前の出力はここで決まる。** 起動直後 (dtb.c が走る前) に
 * 1 文字でも出せないと、実機では何も分からないまま沈黙する。
 * 機械ごとに違うのでビルド時に差せるようにしてある:
 *
 *   make aarch64-kernel8 AARCH64_LOAD_PA=0x80000 \
 *        AARCH64_EARLY_UART=0x3F201000     Raspberry Pi 3 (BCM2837)
 *   make ... AARCH64_EARLY_UART=0xFE201000 Raspberry Pi 4 (BCM2711)
 *
 * **Pi の値は実機の DTB か公式資料で確認すること** (上の注記と同じ)。
 * ここに書いてあるのは「そう聞いている値」であって、実測ではない。
 *
 * DTB が読めればそちらで上書きされる (boot.c)。**表示が (dtb) か (既定値) か
 * を見れば、どちらで動いているか分かる。** */
#ifndef AARCH64_EARLY_UART
#define AARCH64_EARLY_UART              AARCH64_QEMU_VIRT_UART0_BASE
#endif

/* 非セキュア物理タイマの PPI 番号。DTB の timer interrupts が
 * <1 14 0x104> = PPI 14 で、PPI の INTID は +16 なので 30 */
#define AARCH64_TIMER_PPI_DEFAULT       14
#define AARCH64_PPI_INTID_BASE          16

/* PL011 の受信割り込み。QEMU virt の DTB は interrupts = <0 1 4> なので
 * SPI 1、INTID は +32 で 33 (PPI が +16 なのと同じ理屈) */
#define AARCH64_UART_SPI_DEFAULT        1
#define AARCH64_SPI_INTID_BASE          32

/* memory ノードの reg エントリの上限。
 *
 * **Raspberry Pi 4 (4GB) は RAM が 2 つに割れている。**低位 942MB の後ろに
 * ファームウェア/GPU の予約領域があり、そこを飛ばして 0x40000000 から続く。
 * 最初のエントリだけ読むと 942MB しか見えない (2026-08-15 実機で確認)。
 *
 * QEMU virt は 1 つ。4 あれば当面足りる */
#define AARCH64_MAX_MEM_RANGES 4

typedef struct aarch64_boot_info {
    uint64_t dtb_pa;
    /* **memory_base / memory_size は「穴を含む全体」を指す。**
     * base = 最小の base、size = 最後のエントリの終端まで。
     * HHDM のマッピング範囲 (vm.c) はこれを使う。
     * **実際に配ってよいページは mem_range_* のほう** — 穴は RAM では
     * あってもファームウェアの持ち物なので、pmm は配らない */
    uint64_t memory_base;
    uint64_t memory_size;
    uint64_t mem_range_base[AARCH64_MAX_MEM_RANGES];
    uint64_t mem_range_size[AARCH64_MAX_MEM_RANGES];
    uint32_t mem_range_count;
    uint64_t uart_base;
    uint64_t gicd_base;     /* Distributor。DTB の intc reg[0] */
    uint64_t gicd_size;
    uint64_t gicc_base;     /* CPU Interface。**reg[1]**。1 組目だけ読むと落とす */
    uint64_t gicc_size;
    uint64_t first_virtio_mmio_base;
    uint64_t virtio_mmio_stride;
    uint32_t dtb_size;
    uint32_t virtio_mmio_count;
    uint32_t timer_intid;   /* 非セキュア物理タイマ。QEMU virt では 30 */
    /* PL011 の受信割り込み (P3)。DTB の interrupts = <0 1 4> なので
     * SPI 1 -> INTID 33。**これが無いと対話シェルの入力が届かない** */
    uint32_t uart_intid;
    /* virtio-mmio スロットの割り込み。**スロット i の INTID = base + i**。
     * この関係が成り立つことを DTB で確かめてからでないと使えないので、
     * flags の VIRTIO_IRQ_OK が立っているときだけ有効 */
    uint32_t virtio_mmio_irq_base;
    uint32_t cpu_count;
    /* Raspberry Pi 4 の SD カードコントローラ (EMMC2)。**QEMU virt には
     * 無い**ので 0 のまま。番地 0 が「この機械には無い」の印 (virtio と同じ) */
    uint64_t emmc2_base;
    uint64_t emmc2_size;
    uint32_t emmc2_intid;
    /* VideoCore の mailbox (Raspberry Pi)。**画面はこれ経由でしか取れない。**
     * DTB には HDMI の framebuffer が載っていないので、解像度もアドレスも
     * ファームウェアに聞くしかない。**QEMU virt には無い**ので 0 のまま */
    uint64_t mbox_base;
    /* PCIe (ECAM)。**QEMU virt にはあるが、Raspberry Pi 4 の raspi4b には
     * 無い** (QEMU が brcm,bcm2711-pcie を無効にしている)。実機の USB は
     * PCIe の先の VL805 なので、そちらは別途ルートコンプレックスの
     * 初期化が要る。0 = この機械には無い */
    uint64_t pcie_ecam_base;
    uint64_t pcie_ecam_size;
    /* ranges の 32bit MMIO 窓 (child high cell が 0x02000000 のもの)。
     * **BAR はここから割り当てる。**ファームウェアを経由せずに -kernel で
     * 起動すると BAR が未設定なので、自分で配る必要がある */
    uint64_t pcie_mmio_base;
    uint64_t pcie_mmio_size;
    /* **いまはバス 0 しか見ない。** QEMU virt は qemu-xhci をバス 0 に置く。
     * ブリッジの先まで辿るのは必要になってから */
    uint32_t pcie_bus_max;
    /* Raspberry Pi 4 (BCM2711) の PCIe。**ECAM ではない。**
     * 設定空間の出し方が違うので pcie_ecam_base とは別に持つ。
     * この先に VL805 (xHCI) がぶら下がっていて、実機の USB はそこ。
     *
     * **QEMU の raspi4b は PCIe を持っていない** ので、実機でしか 0 以外に
     * ならない */
    uint64_t pcie_brcm_base;
    uint64_t pcie_brcm_size;
    /* 外向きの窓。**PCI から見た番地と CPU の物理が違う** —
     * 実機の DTB では PCI 0xc0000000 が CPU 0x6_00000000 に出る */
    uint64_t pcie_brcm_pci_base;    /* PCI 側 */
    uint64_t pcie_brcm_cpu_base;    /* CPU 側 (物理) */
    uint64_t pcie_brcm_win_size;
    /* 内向き (DMA) の窓。**恒等とは限らない。**
     * 実機は PCI 0x4_00000000 <-> CPU 0x0 で、デバイスに渡す番地は
     * ARM の物理 + (pci_base - cpu_base)。**ここを間違えると
     * 「初期化は通るのに転送だけ動かない」** */
    uint64_t pcie_dma_pci_base;
    uint64_t pcie_dma_cpu_base;
    uint64_t pcie_dma_size;
    uint32_t flags;
} aarch64_boot_info_t;

#define AARCH64_BOOT_FLAG_DTB_VALID       (1U << 0)
#define AARCH64_BOOT_FLAG_DTB_FROM_X0     (1U << 1)  /* 実機はこちら */
#define AARCH64_BOOT_FLAG_DTB_FROM_SCAN   (1U << 2)  /* QEMU の回避策 */
#define AARCH64_BOOT_FLAG_MEMORY_FROM_DTB (1U << 3)
#define AARCH64_BOOT_FLAG_UART_FROM_DTB   (1U << 4)
#define AARCH64_BOOT_FLAG_GIC_FROM_DTB    (1U << 5)
#define AARCH64_BOOT_FLAG_VIRTIO_FROM_DTB (1U << 6)
#define AARCH64_BOOT_FLAG_TIMER_FROM_DTB  (1U << 7)
#define AARCH64_BOOT_FLAG_VIRTIO_IRQ_OK   (1U << 8)  /* base + i が成り立つ */
#define AARCH64_BOOT_FLAG_UART_IRQ_FROM_DTB (1U << 9)
#define AARCH64_BOOT_FLAG_EMMC2_FROM_DTB  (1U << 10)
#define AARCH64_BOOT_FLAG_MBOX_FROM_DTB   (1U << 11)
#define AARCH64_BOOT_FLAG_PCIE_FROM_DTB   (1U << 12)
#define AARCH64_BOOT_FLAG_PCIE_BRCM_FROM_DTB (1U << 13)

/* 直書きの既定値で埋めてから DTB で上書きする。
 * DTB が無くても QEMU virt では動き続ける */
void aarch64_boot_capture(uint64_t dtb_pa);
aarch64_boot_info_t* aarch64_boot_info_mut(void);
const aarch64_boot_info_t* aarch64_boot_info(void);

void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);
void aarch64_uart_puthex64(uint64_t v);
void aarch64_uart_set_base(uint64_t base);

/* 出力の 1 単位を囲む。**この間はタスクが切り替わらない**ので、
 * 並行に走っていても行が割れない。puts / puthex64 は自分で囲んで
 * あるので、**複数回の呼び出しで 1 行を組むときだけ**明示的に要る。
 * 入れ子にできる */
void aarch64_console_begin(void);
void aarch64_console_end(void);
void aarch64_wait_forever(void);

#endif
