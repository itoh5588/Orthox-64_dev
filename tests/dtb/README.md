# 他機の DTB (パーサの検証用)

`kernel/aarch64/dtb.c` を **QEMU virt 以外の木**に対して確かめるために置いてある。
実機が無くても、パーサの解釈結果だけは手元で読める。

## bcm2711-rpi-4-b.dtb

Raspberry Pi 4 Model B。取得元:

    https://github.com/raspberrypi/firmware/raw/master/boot/bcm2711-rpi-4-b.dtb

QEMU virt は周辺をルート直下に置くので `reg` にそのまま物理が入るが、
**Pi は /soc というバスノードの下に置き `reg` にバスアドレスを書く**。
この木でしか通らない経路が 2 つある。

## 使い方

**UART の乗り換えを止めること。** 止めないと、DTB を読んだ瞬間に表示先が
Pi のアドレス (0xfe201000) へ移って何も見えなくなる。

    make aarch64-kernel AARCH64_CFLAGS_EXTRA=-DORTHOX_DTB_KEEP_UART
    qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -smp 1 -nographic \
      -kernel out/kernel-aarch64.elf -dtb tests/dtb/bcm2711-rpi-4-b.dtb

## 期待される答え (2026-08-11 に実測して一致を確認)

    uart      : 0x00000000fe201000  (dtb)
    gic dist  : 0x00000000ff841000  (dtb)
    gic cpu   : 0x00000000ff842000  (dtb)
    cpus      : 0x0000000000000004

真値の出し方 (バスアドレスと ranges から手で計算できる):

    /soc  #address-cells=1  #size-cells=1
    ranges = <0x7e000000 0x0 0xfe000000 0x1800000>
             <0x7c000000 0x0 0xfc000000 0x2000000>
             <0x40000000 0x0 0xff800000 0x0800000>

    serial@7e201000            reg 0x7e201000 -> 0xfe201000
    interrupt-controller@40041000  reg 0x40041000 -> 0xff841000
                                       0x40042000 -> 0xff842000

## この木で見つかった落とし穴

1. **ranges を変換しないと沈黙する。** 0x7e201000 はどこにも繋がって
   いないので、書いてもエラーにならない
2. **status = "disabled" を見ないと無効なポートを掴む。**
   arm,pl011 を名乗るノードが 5 つあり、有効なのは serial@7e201000 だけ。
   見ないと最後に見つけた serial@7e201a00 (0xfe201a00) を採用してしまう
3. **子と親でセル数が違う** (子 1 / 親 2)。揃えて読むと組の境界がずれる

## 未確認 (実機でしか確かめられない)

- **RAM のサイズ。** この DTB の /memory@0 は reg = <0x0 0x0 0x0> で、
  **実機ではファームウェアが起動時に書き換える**。上の実行で 512MB と
  出るのは QEMU が memory ノードを自分の値に差し替えているため
- **Pi 4 の RAM は 0x0 から始まる** (QEMU virt は 0x40000000 から)。
  カーネルのロード先も 0x80000 なので pmm の初期化範囲の前提が変わる
- uart irq = 153 (SPI 121 + 32)。計算は合うが実機では未確認
