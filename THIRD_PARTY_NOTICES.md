# Third-Party Notices / 第三者ソフトウェアの帰属表示

Orthox-64 は独自に設計・実装されたカーネルですが、一部に第三者の
オープンソースソフトウェアを移植・改変・同梱しています。各コンポーネントは
それぞれの著作権者が保持する以下のライセンスの下で提供されています。
本ファイルは、それらのライセンスが要求する著作権表示・許諾表示を保持する
ためのものです。

---

## 1. カーネルに移植・組み込まれているコード

### xv6-riscv （ファイルシステム層）

Orthox-64 の以下のファイルは、MIT の教育用 OS **xv6-riscv** から移植・改変した
派生物です。

- `kernel/xv6fs.c`  ← `xv6-riscv/kernel/fs.c`
- `kernel/xv6bio.c` ← `xv6-riscv/kernel/bio.c`
- `kernel/xv6log.c` ← `xv6-riscv/kernel/log.c`
- 関連ヘッダ `include/xv6fs.h` ほか

各ファイル冒頭に Orthox-64 向けの変更点を記載しています。原著作権表示および
許諾文は以下のとおりです（xv6-riscv の MIT ライセンス）。

```
Copyright (c) 2006-2019 Frans Kaashoek, Robert Morris, Russ Cox,
                        Massachusetts Institute of Technology

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### rpi-boot の `emmc.c` （Raspberry Pi 4 の SD カードドライバ）

`kernel/aarch64/emmc2.c` は、**jncronin/rpi-boot** の `emmc.c` を土台に、
Raspberry Pi 4 の EMMC2 (BCM2711) 向けに書き直したものです。SDHCI の
レジスタ配置・コマンド定義・カード初期化の順序をそちらから取っています。

**リポジトリのルートには `LICENCE.GPL` があり、GitHub の判定も GPL-2.0 に
なりますが、`emmc.c` 自体はファイル冒頭で MIT を宣言しています。**
根拠にすべきは当該ファイルの宣言のほうなので、MIT として扱っています。
**`emmc.c` 以外は一切持ち込んでいません。**

主な相違点（`kernel/aarch64/emmc2.c` の冒頭にも記載）:

- 対象が **EMMC2 (`brcm,bcm2711-emmc2`)**。原典は旧 arasan (`bcm2835-sdhci`) 向け
- VideoCore のメールボックスを使わず、ベースクロックを `CAPABILITIES_0` から読む
- 番地は直書きせず DTB から取る（`/emmc2bus` の `ranges` 変換を通す）
- 転送は PIO のみ。DMA / ADMA は使わない
- Orthox-64 の `storage.h` に合わせた受け口に置き換え

```
Copyright (C) 2013 by John Cronin <jncronin@tysos.org>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

### OpenBSD/NetBSD の `bcmgenet.c` （Raspberry Pi 4 の実 NIC ドライバ）

`kernel/aarch64/genet.c` は、OpenBSD/NetBSD 共通の
`sys/dev/ic/bcmgenet.c` / `bcmgenetreg.h` / `bcmgenetvar.h`
（BCM2711 GENETv5 ドライバ）を土台に、レジスタ配置・リセット手順・
DMA リング構成の組み方を移植したものです。FreeBSD の `genet(4)` も
同系統の実装です。

主な相違点（`kernel/aarch64/genet.c` の冒頭にも記載）:

- mbuf/bus_dma を使わず、固定本数の物理ページ (RX 32 本 / TX 8 本) を
  記述子に一度だけ結び付けて使い回す。scatter-gather は行わない
- MDIO は OpenBSD 版が対応する mii(4) レイヤ経由ではなく、
  IEEE 802.3 clause 22 の標準レジスタだけを直接叩く
- ポーリングのみ (割り込みは未実装)
- PHY のハードリセットは行わない (Pi 4 のブートローダが netboot で
  既にリンクを上げている場合があるため)

```
Copyright (c) 2020 Jared McNeill <jmcneill@invisible.ca>
Copyright (c) 2020 Mark Kettenis <kettenis@openbsd.org>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

---

## 2. 同梱・ビルド時に利用している第三者コンポーネント（`ports/`, `Limine/` ほか）

以下のソフトウェアは、それぞれのソースツリーに含まれるライセンス全文
（`LICENSE` / `COPYING` / `COPYRIGHT` 等）の条件で提供されています。完全な条文は
各ディレクトリのライセンスファイルを参照してください。

| コンポーネント | 用途 | ライセンス | ライセンス全文 |
|---|---|---|---|
| musl libc | ユーザーランド C ライブラリ | MIT | `ports/musl/COPYRIGHT` |
| lwIP | TCP/IP スタック | BSD-3-Clause | `ports/lwip/COPYING` |
| BearSSL | TLS ライブラリ | MIT | `ports/BearSSL/LICENSE.txt` |
| Limine | ブートローダ | BSD-2-Clause | `Limine/COPYING` |
| CPython 3.12.3 | スクリプト言語 | PSF License | `ports/Python-3.12.3/LICENSE` |
| zlib | 圧縮ライブラリ | zlib License | `ports/zlib-pie/` |
| BusyBox | ユーザーランドユーティリティ | **GPL-2.0** | `ports/busybox/LICENSE` |
| doomgeneric | DOOM移植 (`user/doomgeneric/`) | **GPL-2.0** | `user/doomgeneric/LICENSE` |
| GNU Make 4.4.1 | ビルドツール | **GPL-3.0** | `ports/make-4.4.1/COPYING` |
| GNU Binutils 2.26 | アセンブラ/リンカ | **GPL-3.0** | `ports/binutils-2.26/` |
| GCC 4.7.4 | C/C++ コンパイラ | **GPL-3.0 + GCC Runtime Library Exception** | `ports/gcc-4.7.4/COPYING3`, `COPYING.RUNTIME` |

### GPL コンポーネントに関する注意

BusyBox（GPL-2.0）および GNU Make / Binutils / GCC（GPL-3.0）は、コピーレフト
ライセンスです。これらのバイナリを配布する場合は、対応する完全なソースコードの
提供（または入手方法の明示）が必要になります。Orthox-64 本体（カーネルおよび
独自部分）のライセンスはこれらの GPL の影響を受けませんが、配布物に GPL
バイナリを含める際は各ライセンスの義務を満たしてください。

### DOOM のゲームデータ (WAD) について

`rootfs/doom1.wad` は id Software が公式にシェアウェアとして無償配布した
`doom1.wad` (The Ultimate Doom / DOOM の第1エピソード分) です。ソース
コードではなくゲームデータであり、GPL の対象ではありません。id Software
はこのシェアウェア版の複製・再配布を明示的に許可しています。**商用版
(DOOM2.WAD等)はここには含めていません。**

---

## 3. 標準仕様に基づく実装について

ページング、GDT/IDT、Local APIC、PCI コンフィグ機構、xHCI(USB)、ELF ロード、
FAT ファイルシステムなどは、ハードウェア仕様および公開フォーマット仕様
（Intel SDM, xHCI 仕様, PCI 仕様, System V ELF ABI, Microsoft FAT 仕様）に基づき
独自に実装したものであり、特定の他 OS 実装からの複製ではありません。
