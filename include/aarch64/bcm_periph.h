#ifndef ORTHOX_ARCH_AARCH64_BCM_PERIPH_H
#define ORTHOX_ARCH_AARCH64_BCM_PERIPH_H

/* Raspberry Pi 4 (BCM2711) の周辺の番地。
 *
 * **実機の DTB から取った** (tests/dtb/bcm2711-rpi-4-b-live.dtb)。
 * DTB のバス番地 0x7exxxxxx は、CPU から見ると 0xFExxxxxx になる。
 *
 *   /soc/gpio@7e200000     GPIO
 *   /soc/cprman@7e101000   クロック管理 (PWM のクロックはここの #30)
 *   /soc/pwm@7e20c800      PWM1。**3.5mm ジャックはこちら** (PWM0 ではない)
 *
 * **vm.c の写像と sound.c の参照で同じ値を使う。**片方だけ直すと
 * 「番地は合っているのに translation fault」になる */

#define AARCH64_BCM_PERI_BASE   0xFE000000ULL
#define AARCH64_BCM_GPIO_BASE   (AARCH64_BCM_PERI_BASE + 0x200000ULL)
#define AARCH64_BCM_CM_BASE     (AARCH64_BCM_PERI_BASE + 0x101000ULL)
#define AARCH64_BCM_PWM1_BASE   (AARCH64_BCM_PERI_BASE + 0x20C800ULL)
/* legacy DMA (チャネル 0..10)。**PWM の FIFO を CPU の代わりに埋める** (D-3)。
 *   /soc/dma-controller@7e007000  reg = <0x7e007000 0xb00>
 *   同ノードの brcm,dma-channel-mask = 0x7f5 が OS に使えるチャネル
 *     → 0,2,4,5,6,7,8,9,10 (**1 と 3 はファームウェアが押さえている**) */
#define AARCH64_BCM_DMA_BASE    (AARCH64_BCM_PERI_BASE + 0x7000ULL)

/* ---- 電源管理 (PM) — reboot に使う ---------------------------------------
 *
 * **BCM2711 のデータシートに PM ブロックの記載は無い。** BCM2835 から
 * 引き継いだ未文書の領域で、裏は Linux の drivers/watchdog/bcm2835_wdt.c。
 * PWM の DREQ (D-3) と同じで、**出典がデータシートでないことを明記する。**
 *
 * **CM (0xFE101000) の 1 つ下のページ。**音のために CM は張ってあるが
 * PM は別ページなので、vm.c で張り忘れると translation fault になる。
 *
 * Pi 4 には PSCI が無い (DTB は spin-table、psci ノード無し) ので、
 * **watchdog が唯一のリセット手段。**電源は切れないため、xHCI / PCIe は
 * 前回の状態を保ったまま再初期化に入る */
#define AARCH64_BCM_PM_BASE     (AARCH64_BCM_PERI_BASE + 0x100000ULL)
#define AARCH64_BCM_PM_RSTC     0x1CU
#define AARCH64_BCM_PM_RSTS     0x20U
#define AARCH64_BCM_PM_WDOG     0x24U
/* 全書き込みに OR する。**付け忘れると黙って無視される** */
#define AARCH64_BCM_PM_PASSWORD 0x5A000000U
#define AARCH64_BCM_PM_RSTC_WRCFG_CLR       0xFFFFFFCFU
#define AARCH64_BCM_PM_RSTC_WRCFG_FULL_RESET 0x00000020U

#endif
