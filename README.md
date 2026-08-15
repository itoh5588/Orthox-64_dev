# Orthox-64（開発用リポジトリ）

**🇬🇧 English version → [README.en.md](README.en.md)**

このリポジトリは Orthox-64 の**開発用スナップショット**です。書籍版のリファレンス実装は別リポジトリで凍結されています。

**Orthox-64（オーソックス・シックスティフォー）は、自分自身のカーネルを OS の内側でコンパイルできる趣味の x86-64 オペレーティングシステムです。** OS 上にネイティブ移植した GCC ツールチェーンでカーネルをビルドし、Python 3.12 + NumPy、BusyBox、HTTPS まで通る TCP/IP スタック、そして DOOM が動く実用的なユーザーランドを起動します。

起動中の OS が自分のカーネルをソースから再ビルドし、そのカーネルがブートして動作する — **セルフホスティングのループ**を閉じています。

![Orthox-64 デスクトップ](assets/screenshot.png)

## ハイライト

- **セルフホスティング:** ネイティブ移植した GCC 4.7.4 / Binutils 2.26 で、起動中の OS の内側だけで自分のカーネルをビルド・起動できます。
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

x86-64 で作ったカーネルを **aarch64 に移植中**です。**2026 年 8 月 15 日、Raspberry Pi 4 の実機で初めて起動しました。**

![Raspberry Pi 4 実機での初ブート](assets/pi4-first-boot.png)

QEMU で積み上げた MMU とユーザーモードが、**実機で無修正のまま動きました。**

- **起動:** armstub8 経由で EL2 から入り、EL1 に降りて起動（`0x80000` に生バイナリでロード）
- **DTB:** ファームウェアが渡す `bcm2711-rpi-4-b.dtb` を解釈し、UART / GIC-400 / EMMC2 の番地と IRQ を実機の値で取得
- **MMU:** 4KB granule / VA 39bit、TTBR1 にカーネル、恒等マップを外して高位 VA で走行
- **EL0:** 2 つのアドレス空間でユーザープロセスを実行、permission fault からの復帰、コンテキストスイッチ、タイマー起床までのスケジューラ

**未対応:** SD カード（EMMC2）の初期化、実機 RAM 容量の取得。

手順と実機の値は [`scripts/pi4/README.md`](scripts/pi4/README.md) にあります。

## ライセンス

Orthox-64 本体は MIT ライセンス（[LICENSE](LICENSE)）。カーネルには xv6-riscv（MIT）由来のコードを含み、`ports/` に musl・lwIP・BearSSL・Limine・CPython・zlib・BusyBox・GNU Make/Binutils/GCC などを同梱しています。ライセンスと配布上の注意は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照。

## 謝辞

- **[MikanOS](https://github.com/uchan-nos/mikanos)**（[uchan-nos](https://github.com/uchan-nos) 氏）: カーネルアーキテクチャの参考。
- **[Limine](https://github.com/limine-bootloader/limine)**: ブートローダー。
- **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)**（MIT PDOS, MIT）: ルートファイルシステム（xv6fs）の移植元。
