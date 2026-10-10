#ifndef ORTHOX_ARCH_RISCV64_BOOT_H
#define ORTHOX_ARCH_RISCV64_BOOT_H

#include <stdint.h>

struct task;

#define RISCV64_QEMU_VIRT_UART0_BASE   0x10000000ULL
#define RISCV64_QEMU_VIRT_CLINT_BASE   0x02000000ULL
#define RISCV64_QEMU_VIRT_PLIC_BASE    0x0C000000ULL
#define RISCV64_QEMU_VIRT_VIRTIO_BASE  0x10001000ULL
#define RISCV64_QEMU_VIRT_VIRTIO_STRIDE 0x00001000ULL
/* QEMU virt/OpenSBI の timebase。DTB に timebase-frequency が無いときの既定値 */
#define RISCV64_QEMU_VIRT_TIMEBASE_HZ  10000000ULL

/* コンソール UART の種類。DTB の compatible で決める */
#define RISCV64_UART_KIND_NS16550  0U  /* QEMU virt */
#define RISCV64_UART_KIND_UARTLITE 1U  /* AMD AXI UART Lite (Arty S7 MicroBlaze V) */

typedef struct riscv64_boot_info {
    uint64_t hart_id;
    uint64_t dtb_pa;
    uint64_t memory_base;
    uint64_t memory_size;
    uint64_t uart_base;
    uint64_t first_virtio_mmio_base;
    uint64_t timebase_hz;
    uint64_t initrd_start;   /* DTB /chosen linux,initrd-start (0 = 無し) */
    uint64_t initrd_end;
    uint64_t intc_base;      /* AXI INTC (0 = 無し) */
    uint32_t uart_irq;       /* コンソール UART の INTC 入力番号 */
    uint32_t dtb_size;
    uint32_t virtio_mmio_count;
    uint32_t flags;
    uint32_t uart_kind;
} riscv64_boot_info_t;

#define RISCV64_BOOT_FLAG_DTB_VALID  (1U << 0)
#define RISCV64_BOOT_FLAG_UART_READY (1U << 1)
#define RISCV64_BOOT_FLAG_UART_FROM_DTB (1U << 2)
#define RISCV64_BOOT_FLAG_VIRTIO_MMIO_FOUND (1U << 3)
#define RISCV64_BOOT_FLAG_PLIC_FOUND (1U << 4)
#define RISCV64_BOOT_FLAG_INTC_FOUND (1U << 5)
#define RISCV64_BOOT_FLAG_UART_IRQ   (1U << 6)

/*
 * DTB の cpu ノードに orthox,no-wfi があれば wfi を使わない。
 * Arty S7 の MicroBlaze V は Wakeup 入力が 0 だと wfi から二度と戻らず、
 * デバッガからも止められない。S モードのタイマ (Sstc) には Wakeup に
 * つなげる外部信号が無いので、wfi を待たずに回る。
 */
extern volatile int g_riscv64_no_wfi;

static inline void riscv64_wfi(void) {
    if (!g_riscv64_no_wfi) __asm__ volatile("wfi" ::: "memory");
}

void riscv64_boot_capture(uint64_t hart_id, uint64_t dtb_pa);
const riscv64_boot_info_t* riscv64_boot_info(void);
/* time CSR の周波数 (Hz)。DTB の /cpus/timebase-frequency、無ければ 10MHz */
uint64_t riscv64_timebase_hz(void);
/* DTB に PLIC があるか。無い基板ではコンソール入力をタイマーでポーリングする */
int riscv64_has_plic(void);
/* DTB に AXI INTC があり、コンソール UART の割り込み番号も分かっているか */
int riscv64_has_axi_intc(void);
void riscv64_uart_init(void);
void riscv64_uart_putchar(char ch);
int riscv64_uart_getchar_nonblock(void);
void riscv64_console_poll_input(void);
int riscv64_console_read(char* buf, int count);
int riscv64_console_read_or_wait(char* buf, int count, struct task* self);
int riscv64_console_has_input(void);
/* 待ち行列へ登録する。戻り値: 登録できたら 1、溢れたら 0 */
int riscv64_console_set_waiter(struct task* t);
void riscv64_console_clear_waiter(struct task* t);
void riscv64_uart_puts(const char* s);
void riscv64_uart_puthex64(uint64_t value);
void riscv64_mark_user_handoff_started(void);
int riscv64_user_handoff_started(void);
void riscv64_wait_forever(void);

// SMP (kernel/riscv64/smp.c)
void riscv64_smp_start_secondaries(void);
uint32_t riscv64_smp_hart_count(void);
uint64_t riscv64_smp_hartid(uint32_t cpu_index);
void riscv64_uart_enable_rx_interrupt(void);
void riscv64_early_main(uint64_t hart_id, uint64_t dtb_pa);

#endif
