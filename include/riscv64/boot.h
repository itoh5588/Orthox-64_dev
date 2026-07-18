#ifndef ORTHOX_ARCH_RISCV64_BOOT_H
#define ORTHOX_ARCH_RISCV64_BOOT_H

#include <stdint.h>

struct task;

#define RISCV64_QEMU_VIRT_UART0_BASE   0x10000000ULL
#define RISCV64_QEMU_VIRT_CLINT_BASE   0x02000000ULL
#define RISCV64_QEMU_VIRT_PLIC_BASE    0x0C000000ULL
#define RISCV64_QEMU_VIRT_VIRTIO_BASE  0x10001000ULL
#define RISCV64_QEMU_VIRT_VIRTIO_STRIDE 0x00001000ULL

typedef struct riscv64_boot_info {
    uint64_t hart_id;
    uint64_t dtb_pa;
    uint64_t memory_base;
    uint64_t memory_size;
    uint64_t uart_base;
    uint64_t first_virtio_mmio_base;
    uint32_t dtb_size;
    uint32_t virtio_mmio_count;
    uint32_t flags;
} riscv64_boot_info_t;

#define RISCV64_BOOT_FLAG_DTB_VALID  (1U << 0)
#define RISCV64_BOOT_FLAG_UART_READY (1U << 1)
#define RISCV64_BOOT_FLAG_UART_FROM_DTB (1U << 2)
#define RISCV64_BOOT_FLAG_VIRTIO_MMIO_FOUND (1U << 3)

void riscv64_boot_capture(uint64_t hart_id, uint64_t dtb_pa);
const riscv64_boot_info_t* riscv64_boot_info(void);
void riscv64_uart_init(void);
void riscv64_uart_putchar(char ch);
int riscv64_uart_getchar_nonblock(void);
void riscv64_console_poll_input(void);
int riscv64_console_read(char* buf, int count);
void riscv64_console_set_waiter(struct task* t);
void riscv64_console_clear_waiter(struct task* t);
void riscv64_uart_puts(const char* s);
void riscv64_uart_puthex64(uint64_t value);
void riscv64_mark_user_handoff_started(void);
int riscv64_user_handoff_started(void);
void riscv64_wait_forever(void);
void riscv64_early_main(uint64_t hart_id, uint64_t dtb_pa);

#endif
