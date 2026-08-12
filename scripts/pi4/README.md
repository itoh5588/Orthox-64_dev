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

**変換器は 3.3V のものを使う。5V を GPIO に入れると Pi が壊れる。**
Pi 側には電源を挿さない (変換器の VCC は繋がない)。

`config.txt` の `uart_2ndstage=1` でファームウェア自身のログも出る。
**最初の 1 回は必ず入れること** — カーネルから 1 文字も出ないときに
「配線が悪い」のか「カーネルが動いていない」のかを切り分けられる。

ホスト側 (WSL からは USB シリアルが見えないことがある。見えなければ
Windows 側の端末を使う):

    screen /dev/ttyUSB0 115200        (抜けるのは Ctrl-A K)
    または  picocom -b 115200 /dev/ttyUSB0

## 初回起動で見るもの (上から順に)

**いきなりカーネルまで行かない。** どこで止まったかで原因が分かれる。

| # | 出るはずのもの | 出なければ疑う所 |
|---|---|---|
| 1 | ファームウェアのログ | 配線 / `config.txt` / SD の中身。**カーネル以前** |
| 2 | `--- Orthox-64 aarch64 boot ---` | 早期 UART の番地、ロード先、`arm_64bit=1` |
| 3 | `CurrentEL : EL1  (入口 ELx)` | — (**入口 EL がここで分かる**。armstub 経由なら EL2) |
| 4 | `aarch64-dtb-ok` までの各行 | DTB の解釈。`(dtb)` か `(既定値)` かを見る |
| 5 | `memory : ...` | **実機ではファームウェアが DTB を書き換える**ので |
|   |  | `(dtb)` で実機の容量が出るはず。`(既定値)` なら 512MB に退いている |
| 6 | `aarch64-timer-ok` / `sleep ... ok` | GIC-400 |
| 7 | `emmc2 : 初期化 ok` | **SD カード。QEMU では配線が違って確かめられなかった所** |

**2 が出ない場合、まず 1 が出ているかを見る。** 1 も出ていなければ
配線かカードの問題で、カーネルは無実。

## 未確認 (QEMU では確かめられていない)

- **EMMC2 (0xFE340000) で本当にカードが読めるか。**
  QEMU の raspi4b は SD カードを旧 sdhci (0xFE300000) に繋いでいて
  EMMC2 は空なので、**実機でしか分からない** (日報2026-08-13 §6)
- **UART の受信割り込み (SPI 121 -> INTID 153)。**
  `GICD_ITARGETSR` を入れた効果は実機の 4 コアでないと出ない (同 §4)
- RAM のサイズ。上の表の 5

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

判定つきで回すなら:

    make aarch64-pi4-smoke

起動形式 / DTB (ranges と status) / GIC-400 のタイマ割り込み / MMU / EL0 まで
見る。**raspi4b を持つ qemu が無ければ SKIP** して失敗にしない
(環境が無いことと、カーネルが壊れていることを混ぜない)。

**`-dtb` は必須。** 付けないと x0 に 0x100 が入って DTB が渡らない。
実機はファームウェアが `bcm2711-rpi-4-b.dtb` を読んで渡すので、
付けたほうが実機に近い。

## 既知の未確認 (実機かファームウェア込みでしか確かめられない)

- **RAM のサイズ。** 配布 DTB の /memory@0 は reg = <0x0 0x0 0x0> で、
  実機ではファームウェアが起動時に書き換える (tests/dtb/README.md)
- **Pi 4 の RAM は 0x0 から始まる** (QEMU virt は 0x40000000 から)。
  カーネルのロード先も 0x80000 なので pmm の初期化範囲の前提が変わる
- uart irq = 153 (SPI 121 + 32)。計算は合うが未確認
