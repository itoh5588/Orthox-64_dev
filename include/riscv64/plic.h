#ifndef ORTHOX_ARCH_RISCV64_PLIC_H
#define ORTHOX_ARCH_RISCV64_PLIC_H

#include <stdint.h>

/*
 * SiFive PLIC (QEMU virt の外部割り込みコントローラ)。
 *
 * これが無いと SEIE を立てられず、UART 受信も virtio も割り込みで拾えない。
 * 以前はドライバ自体が存在せず、コンソール入力は kernel_yield() のたびに
 * 受信 FIFO を舐めるポーリングだった。
 */

/* QEMU virt の割り込み番号 */
#define RISCV64_IRQ_VIRTIO0   1   /* virtio-mmio スロット 0..7 = IRQ 1..8 */
#define RISCV64_IRQ_UART0    10

/* boot hart で 1 回。優先度の設定など全体の初期化 */
void riscv64_plic_init_global(void);
/* hart ごとに 1 回。しきい値と有効化ビット */
void riscv64_plic_init_hart(void);
/* 割り込み源の優先度を設定する (0 = 無効)。有効化ビットとセットで要る */
void riscv64_plic_set_priority(uint32_t irq, uint32_t priority);
/* 割り込み源を有効にする (現在の hart のコンテキストに対して) */
void riscv64_plic_enable_irq(uint32_t irq);
/* 保留中の割り込みを 1 つ受け取る。0 = 何も無い */
uint32_t riscv64_plic_claim(void);
/* 処理完了を通知する。claim した番号をそのまま渡すこと */
void riscv64_plic_complete(uint32_t irq);

#endif
