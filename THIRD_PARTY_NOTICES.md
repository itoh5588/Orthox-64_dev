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
| GNU Make 4.4.1 | ビルドツール | **GPL-3.0** | `ports/make-4.4.1/COPYING` |
| GNU Binutils 2.26 | アセンブラ/リンカ | **GPL-3.0** | `ports/binutils-2.26/` |
| GCC 4.7.4 | C/C++ コンパイラ | **GPL-3.0 + GCC Runtime Library Exception** | `ports/gcc-4.7.4/COPYING3`, `COPYING.RUNTIME` |

### GPL コンポーネントに関する注意

BusyBox（GPL-2.0）および GNU Make / Binutils / GCC（GPL-3.0）は、コピーレフト
ライセンスです。これらのバイナリを配布する場合は、対応する完全なソースコードの
提供（または入手方法の明示）が必要になります。Orthox-64 本体（カーネルおよび
独自部分）のライセンスはこれらの GPL の影響を受けませんが、配布物に GPL
バイナリを含める際は各ライセンスの義務を満たしてください。

---

## 3. 標準仕様に基づく実装について

ページング、GDT/IDT、Local APIC、PCI コンフィグ機構、xHCI(USB)、ELF ロード、
FAT ファイルシステムなどは、ハードウェア仕様および公開フォーマット仕様
（Intel SDM, xHCI 仕様, PCI 仕様, System V ELF ABI, Microsoft FAT 仕様）に基づき
独自に実装したものであり、特定の他 OS 実装からの複製ではありません。
