# Orthox-64（開発用リポジトリ）

**🇬🇧 English version → [README.en.md](README.en.md)**

このリポジトリは Orthox-64 の**開発用スナップショット**です。書籍版のリファレンス実装は別リポジトリで凍結されています。

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
- **SMP:** QEMU 上で 4 CPU 起動、LAPIC タイマー、per-CPU run queue、blocking wakeup 経路を検証済み。
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

## Raspberry Pi 4 (aarch64) への移植

x86-64 で作ったカーネルを **aarch64 へ移植**しました。**2026 年 8 月 15 日に Raspberry Pi 4 の実機で初めて起動**し、**2026 年 8 月 23 日、実機上でセルフホスティングのループを閉じました。**

![Raspberry Pi 4 実機での初ブート](assets/pi4-first-boot.png)

### 実機でのセルフホスティング

Raspberry Pi 4 の上で動く Orthox が、**自分のカーネルをソースからビルドし、そのカーネルで起動します。**

- OS 内の GCC 4.7.4 と Binutils が、カーネル 41 本の C と 5 本のアセンブラ（24,227 行）を **約 50 分**でコンパイル・リンク
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
