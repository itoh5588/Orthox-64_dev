# Orthox-64（開発用リポジトリ）

**🇬🇧 English version → [README.en.md](README.en.md)**

このリポジトリは Orthox-64 の**開発用スナップショット**です（現在 **v0.7.0**）。書籍版のリファレンス実装は別リポジトリで凍結されています。

**Orthox-64（オーソックス・シックスティフォー）は、自分自身のカーネルを OS の内側でコンパイルできる趣味のオペレーティングシステムです。x86-64 / aarch64 / RISC-V (riscv64) の 3 つの ISA で動きます。** OS 上にネイティブ移植した GCC ツールチェーンでカーネルをビルドし、Python 3.12 + NumPy、BusyBox、HTTPS まで通る TCP/IP スタック、そして DOOM が動く実用的なユーザーランドを起動します。

起動中の OS が自分のカーネルをソースから再ビルドし、そのカーネルがブートして動作する — **セルフホスティングのループ**を閉じています。

| ISA | セルフホスティング |
|---|---|
| **x86-64** | OS が自分のカーネルをビルドして起動 |
| **aarch64** | OS が自分のカーネルをビルドして起動。**Raspberry Pi 4 の実機で成立**（2026-08-23） |
| **riscv64** | **OS 上の GCC が GCC 自身をビルド**（2026-08-03）。生成コードはクロス版と `.ident` 1 行を除いて完全一致 |

![Orthox-64 デスクトップ](assets/screenshot.png)

## ハイライト

- **セルフホスティング:** ネイティブ移植した GCC 4.7.4 / Binutils 2.26 で、起動中の OS の内側だけで自分のカーネルをビルド・起動できます（上表）。
- **動的リンク対応ユーザーランド:** musl の動的リンカによる `.so` ロード、`dlopen`/`dlsym`、TLS、C++ ランタイム対応。Python 3.12 が NumPy 1.26.4 を import・実行できます。
- **ネットワーク:** `virtio-net` + `lwIP` の IPv4 スタック（DHCP / DNS / ICMP / UDP / TCP / socket）、BusyBox `httpd`、BearSSL による HTTPS クライアント。
- **SMP:** 3 つの ISA すべてで 4 CPU 起動。per-CPU run queue、blocking wakeup 経路を検証済み。**fork は 3 アーキとも copy-on-write** で、判断は共通層・PTE と TLB はアーキ側という分担です。
- **DOOM**（`doomgeneric`）も動作します。

## クイックスタート

リファレンスホストは **Ubuntu 24.04（WSL2 含む）** または macOS。ホスト側ビルドは `clang -target x86_64-elf` + `lld` を使うため、専用のクロス GCC は不要です。

```bash
# 1. ビルド依存パッケージ（Ubuntu 22.04 / 24.04 / WSL2）
sudo apt-get update
sudo apt-get install -y clang lld llvm build-essential make python3 \
  xorriso mtools qemu-system-x86 git

# 2. ビルド（orthos.iso が生成される）
make

# 3. QEMU で起動（Ctrl-A x で終了）
make run
```

詳細な手順・macOS ビルド・OS 上 GCC 4.7.4 ツールチェーンのビルドは [INSTALL.jp.md](INSTALL.jp.md)（英語版: [INSTALL.md](INSTALL.md)）を参照。

## 主な構成

- **カーネル:** 64bit ロングモード、Limine ブート、PMM/VMM ページング、プリエンプティブ・マルチタスク、SMP scheduler。
- **ファイルシステム:** VFS + Read-Write な xv6fs（xv6-riscv から移植、triple-indirect ブロックで最大 ~16 GB/ファイル）。
- **移植済み:** musl 1.2.5 / BusyBox 1.27 / Binutils 2.26 / GCC 4.7.4 / Python 3.12.3 / NumPy 1.26.4 / doomgeneric。

## 3 アーキ間の syscall 実装統合

x86-64 / aarch64 / riscv64 は元々 syscall 周りの実装系統が分かれており、
同じ syscall が最大 3 通りに別実装されていた。共通する 60 種の syscall の
うち、**59 種を単一実装に統合済み**（`fstat` は `struct stat` の
フィールド並びが ISA ごとに異なる ABI 差のため対象外。これで統合対象は
すべて完了）。

最後まで残っていた `ioctl` は、フォアグラウンドプロセスグループ
(`TIOCGPGRP`/`TIOCSPGRP`)・termios 本体 (`TCGETS`/`TCSETS`)・
`FIOCLEX`/`FIONCLEX`・コンソール tty の判定 (`fd_is_console`) の
4 つの機能差を順に統合した。termios 本体は x86 (`orth_termios`,
`c_cc[20]`) と aarch64/riscv64 (`c_line` + `c_cc[32]`) で別レイアウトに
見えたが、3 アーキの musl の `struct termios` を突き合わせると `c_line`
+ `c_cc[NCCS=32]` で完全に同一で、x86 側がその ABI とそもそも一致して
いなかっただけだった。

統合の過程で、アーキ間の食い違いに起因する実際の不具合も複数見つかり
修正した。代表例:

- `getcwd` の返り値が POSIX の規約（成功時は長さ、失敗時は `-errno`）に
  反し、ポインタを返していた（musl 越しに `ENOENT` へ化ける不具合）
- x86 の `nanosleep` が、指定した待ち時間より早く返ることがあった
- x86 の RTC (`clock_gettime(CLOCK_REALTIME)`) が閏年の数え方の誤りで、
  平年の日付を 1 日遅く計算していた

## メモリ管理とタスク切り替えの共通化

syscall に続いて、**fork の copy-on-write と物理ページの配り方**も 3 アーキ共通層へ移しました。分担は Linux に倣い、**判断を共通層に、PTE の操作と TLB の破棄をアーキ側に**置いています。

- `kernel/vm_cow.c` — fork でページを共有する判断と、書き込みフォルトの後始末
- `kernel/pmm_core.c` — ビットマップ・参照カウント・探し方。アーキ側は範囲と置き場を渡すだけ

効果（Raspberry Pi 4 実機、fork 1 回あたりの中央値、n=1000）:

| | 全ページを写す方式 | copy-on-write |
|---|---|---|
| fork → 子がすぐ終了 | 2.46 ms | **0.25 ms** |
| fork → 子がすぐ終了（4MB の作業領域） | 11.18 ms | **0.61 ms** |
| fork → 親が後で書く（同上） | 11.23 ms | **1.13 ms** |

あわせて、**SMP で fork/exit を繰り返すと壊れる箇所**を洗い出して直しました。ユーザーへ降りる入口と trap の出口で割り込みが開いていた件、`task_list` をロック無しで辿っていた件、CPU から降りきる前の zombie を回収していた件、そしてタスク切り替えの出口が割り込みフラグ（`IF`, ビット 9）ではなくトラップフラグ（`TF`, ビット 8）を落としていてデッドロックを招いていた件などです。

## Raspberry Pi 4 (aarch64) への移植

x86-64 で作ったカーネルを **aarch64 へ移植**しました。**2026 年 8 月 15 日に Raspberry Pi 4 の実機で初めて起動**し、**2026 年 8 月 23 日、実機上でセルフホスティングのループを閉じました。**

![Raspberry Pi 4 実機での初ブート](assets/pi4-first-boot.png)

### 実機でのセルフホスティング

Raspberry Pi 4 の上で動く Orthox が、**自分のカーネルをソースからビルドし、そのカーネルで起動します。**

- OS 内の GCC 4.7.4 と Binutils が、カーネル 41 本の C と 5 本のアセンブラ（24,227 行）をコンパイル・リンク。**初成立時（2026-08-23）は約 50 分**でしたが、`-pipe` と並列ビルドが効くようになり **`make -j4` で 46 秒**（2026-09-19 実測）
- できたカーネルを起動 — USB・SD カード・シェルまですべて動作
- **そのカーネルの上でもう一度ビルドすると、1 回目とバイト単位で一致**（217,088 バイト）。**安定した不動点**であることを確認済み

x86-64 に続き、**カーネルのセルフホスティングが 2 つ目の ISA で成立**しました。しかも QEMU ではなく実機です。

### 実機で動いているもの

- **起動:** armstub8 経由で EL2 から入り EL1 へ。LAN からの netboot にも対応
- **MMU:** 4KB granule / VA 39bit、TTBR1 にカーネル、高位 VA で走行
- **ストレージ:** EMMC2 で SD カードを読み書き、MBR → xv6fs
- **画面:** HDMI にテキストコンソールとフレームバッファ
- **USB:** VL805 (xHCI) を PCIe 越しに初期化、キーボード入力
- **音:** PWM + DMA で 3.5mm ジャックへ出力
- **DOOM:** 実機で動作。効果音と音楽つき

手順と実機の値は [`scripts/pi4/README.md`](scripts/pi4/README.md) にあります。

## ライセンス

Orthox-64 本体は MIT ライセンス（[LICENSE](LICENSE)）。カーネルには **xv6-riscv（MIT）** と **rpi-boot の `emmc.c`（MIT）** 由来のコードを含み、`ports/` に musl・lwIP・BearSSL・Limine・CPython・zlib・BusyBox・GNU Make/Binutils/GCC などを同梱しています。ライセンスと配布上の注意は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照。

## 謝辞

- **[MikanOS](https://github.com/uchan-nos/mikanos)**（[uchan-nos](https://github.com/uchan-nos) 氏）: カーネルアーキテクチャの参考。
- **[Limine](https://github.com/limine-bootloader/limine)**: ブートローダー。
- **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)**（MIT PDOS, MIT）: ルートファイルシステム（xv6fs）の移植元。
- **[rpi-boot](https://github.com/jncronin/rpi-boot)**（[John Cronin](https://github.com/jncronin) 氏, MIT）: Raspberry Pi の SD カード（SDHCI）ドライバの移植元。
