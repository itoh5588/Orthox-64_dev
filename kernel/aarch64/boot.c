/*
 * AArch64 (QEMU virt) の早期起動。M0: PL011 に文字が出るところまで。
 *
 * アドレスは QEMU が吐いた DTB から取った実測値 (推測ではない):
 *
 *   memory@40000000   0x40000000  size 0x20000000 (512MB)
 *   pl011@9000000     0x09000000  compatible = arm,pl011
 *   intc@8000000      0x08000000  compatible = arm,cortex-a15-gic (GICv2)
 *   virtio_mmio@...   0x0a000000  0x200 刻みで 32 スロット
 *
 * ここで表示する CurrentEL は決め打ちにしない。QEMU virt は既定
 * (virtualization=off) だと EL1、virtualization=on だと EL2 で始まる。
 * どちらで来ているかは M1 (例外ベクタ / EL2->EL1 の降格) の前提になる。
 */
#include <stdint.h>

/* PL011 UART。QEMU は初期化なしでも DR に書けば出るが、実機と手順を
 * 揃えておく (M1 以降で割り込み受信を足すときに効く) */
#define PL011_BASE  0x09000000UL
#define PL011_DR    (PL011_BASE + 0x00)
#define PL011_FR    (PL011_BASE + 0x18)
#define PL011_FR_TXFF  (1U << 5)   /* 送信 FIFO が満杯 */

static inline void mmio_write32(uint64_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = v;
}

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

void aarch64_uart_putchar(char c) {
    while (mmio_read32(PL011_FR) & PL011_FR_TXFF) {
        /* 送信 FIFO が空くまで待つ */
    }
    mmio_write32(PL011_DR, (uint32_t)(unsigned char)c);
}

void aarch64_uart_puts(const char* s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') aarch64_uart_putchar('\r');
        aarch64_uart_putchar(*s);
        s++;
    }
}

static void put_hex64(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xfU];
    }
    buf[18] = '\0';
    aarch64_uart_puts(buf);
}

static uint64_t read_current_el(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return v >> 2;   /* [3:2] が EL */
}

static uint64_t read_mpidr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

void aarch64_wait_forever(void);

void aarch64_early_main(uint64_t dtb_phys) {
    aarch64_uart_puts("\n--- Orthox-64 aarch64 boot ---\n");

    aarch64_uart_puts("  CurrentEL : EL");
    aarch64_uart_putchar((char)('0' + (int)(read_current_el() & 3U)));
    aarch64_uart_puts("\n");

    aarch64_uart_puts("  MPIDR_EL1 : ");
    put_hex64(read_mpidr());
    aarch64_uart_puts("\n");

    aarch64_uart_puts("  DTB       : ");
    put_hex64(dtb_phys);
    aarch64_uart_puts("\n");

    /* .bss が実際に 0 で埋まっているかを見る。start.S の埋め方を間違えると
     * ここから先の C が静かに壊れるので、先に確かめておく */
    {
        static uint64_t bss_probe;
        aarch64_uart_puts("  bss zero  : ");
        aarch64_uart_puts(bss_probe == 0 ? "ok" : "BAD");
        aarch64_uart_puts("\n");
    }

    aarch64_uart_puts("aarch64-boot-ok\n");
    aarch64_wait_forever();
}
