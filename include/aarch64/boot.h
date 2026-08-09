#ifndef ORTHOX_ARCH_AARCH64_BOOT_H
#define ORTHOX_ARCH_AARCH64_BOOT_H

#include <stdint.h>

/* DTB が読めなかったときに退く値。**QEMU virt の実測値** であって、
 * Raspberry Pi 4 では全部違う (段取り 3 で効いてくる)。
 *
 *   Pi 4 の PL011 は 0xFE201000、GIC-400 は 0xFF841000 付近。
 *   **記憶で書かず、実機の DTB か公式資料で確認すること** (日報2026-08-09 E)
 */
#define AARCH64_QEMU_VIRT_RAM_BASE      0x40000000ULL
#define AARCH64_QEMU_VIRT_RAM_SIZE      0x20000000ULL   /* 512MB */
#define AARCH64_QEMU_VIRT_UART0_BASE    0x09000000ULL
#define AARCH64_QEMU_VIRT_GICD_BASE     0x08000000ULL
#define AARCH64_QEMU_VIRT_GICC_BASE     0x08010000ULL
#define AARCH64_QEMU_VIRT_GIC_SIZE      0x00010000ULL
#define AARCH64_QEMU_VIRT_VIRTIO_BASE   0x0a000000ULL
#define AARCH64_QEMU_VIRT_VIRTIO_STRIDE 0x00000200ULL

/* 非セキュア物理タイマの PPI 番号。DTB の timer interrupts が
 * <1 14 0x104> = PPI 14 で、PPI の INTID は +16 なので 30 */
#define AARCH64_TIMER_PPI_DEFAULT       14
#define AARCH64_PPI_INTID_BASE          16

typedef struct aarch64_boot_info {
    uint64_t dtb_pa;
    uint64_t memory_base;
    uint64_t memory_size;
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
    /* virtio-mmio スロットの割り込み。**スロット i の INTID = base + i**。
     * この関係が成り立つことを DTB で確かめてからでないと使えないので、
     * flags の VIRTIO_IRQ_OK が立っているときだけ有効 */
    uint32_t virtio_mmio_irq_base;
    uint32_t cpu_count;
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

/* 直書きの既定値で埋めてから DTB で上書きする。
 * DTB が無くても QEMU virt では動き続ける */
void aarch64_boot_capture(uint64_t dtb_pa);
aarch64_boot_info_t* aarch64_boot_info_mut(void);
const aarch64_boot_info_t* aarch64_boot_info(void);

void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);
void aarch64_uart_puthex64(uint64_t v);
void aarch64_uart_set_base(uint64_t base);
void aarch64_wait_forever(void);

#endif
