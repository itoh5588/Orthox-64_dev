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

#endif
