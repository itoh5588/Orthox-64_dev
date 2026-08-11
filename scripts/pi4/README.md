# Raspberry Pi 4 で起動するための一式

`make aarch64-pi4-boot` で `out/pi4-boot/` に組み立てる。
**QEMU の raspi4b でも実機でも同じものを使う。**

## 中身

    kernel8.img   0x80000 にリンクした生バイナリ (ELF ではない)
    config.txt    ファームウェアへの指示。scripts/pi4/config.txt の写し

## 実機で使う

SD カードの**先頭パーティション (FAT32 / boot)** の直下に置く。
Raspberry Pi OS のカードなら、そこの config.txt を退避して差し替え、
kernel8.img を置くだけでよい (ファームウェア一式は既にある)。

空のカードから作る場合は、別途ファームウェアが要る:

    start4.elf  fixup4.dat  bcm2711-rpi-4-b.dtb
    (https://github.com/raspberrypi/firmware の boot/ から取れる。
     Pi 4 に bootcode.bin は要らない — SPI EEPROM から起動するため)

## シリアルの見方

**HDMI には何も出ない。** シリアルが唯一の出力。

    GPIO14 (pin 8)  = Pi の TX -> 変換器の RX
    GPIO15 (pin 10) = Pi の RX -> 変換器の TX
    GND    (pin 6)
    115200 8N1

`config.txt` の `uart_2ndstage=1` でファームウェア自身のログも出る。
**最初の 1 回は必ず入れること** — カーネルから 1 文字も出ないときに
「配線が悪い」のか「カーネルが動いていない」のかを切り分けられる。

## なぜ config.txt に disable-bt が要るか

**Pi 4 の PL011 (UART0) は既定で Bluetooth に取られている。**
GPIO14/15 に出ているのは mini-UART のほう。`dtoverlay=disable-bt` で
Bluetooth を外すと PL011 が GPIO14/15 に回る。

これで**カーネル側は GPIO の alt-func もクロックも触らなくてよい**
(ファームウェアがやる)。洗い出しの項目 E がほぼ不要になる。

## QEMU の raspi4b で使う

    qemu-system-aarch64 -machine raspi4b -nographic \
      -kernel out/pi4-boot/kernel8.img -dtb tests/dtb/bcm2711-rpi-4-b.dtb

**QEMU 9.x 以降が要る。** 8.2 には raspi4b が無い (raspi3b まで)。
raspi3b でも起動形式 (0x80000 / 生バイナリ / EL2) の確認はできるが、
**割り込みコントローラと周辺は Pi 4 と別物**なのでそこから先は見られない。

## 既知の未確認 (実機かファームウェア込みでしか確かめられない)

- **RAM のサイズ。** 配布 DTB の /memory@0 は reg = <0x0 0x0 0x0> で、
  実機ではファームウェアが起動時に書き換える (tests/dtb/README.md)
- **Pi 4 の RAM は 0x0 から始まる** (QEMU virt は 0x40000000 から)。
  カーネルのロード先も 0x80000 なので pmm の初期化範囲の前提が変わる
- uart irq = 153 (SPI 121 + 32)。計算は合うが未確認
