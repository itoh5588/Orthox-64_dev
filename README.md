# Orthox-64

**🇬🇧 English version → [README.en.md](README.en.md)**

**Orthox-64（オーソックス・シックスティフォー）は、自分自身のカーネルを OS の内側でコンパイルできる趣味の x86-64 オペレーティングシステムです。** OS 上にネイティブ移植した GCC ツールチェーンでカーネルをビルドし、Python 3.12 + NumPy、BusyBox、HTTPS まで通る TCP/IP スタック、そして DOOM が動く実用的なユーザーランドを起動します。

多くの趣味 OS はシェルで止まります。Orthox-64 は**セルフホスティングのループ**を閉じました。起動中の OS が自分のカーネルをソースから再ビルドし、そのカーネルがブートして動作します。

![Orthox-64 デスクトップ](assets/screenshot.png)
![Orthox-64 DOOM](assets/doom.png)

## ハイライト

- **セルフホスティング達成（Day 43, 2026-05-03）:** Orthox-64 は、ネイティブ移植した GCC 4.7.4 / Binutils 2.26 ツールチェーンを使い、*起動中の OS の内側だけで*自分のカーネルをソースからコンパイルできます。ビルドされたカーネルは正常に起動・動作します。セルフホスティングのビルドループが閉じ、OS が自分自身をビルドします。
- **本物の動的リンク対応ユーザーランド:** musl の動的リンカによる完全な動的リンク — 位置独立 `.so` のロード、`dlopen`/`dlsym`、TLS、C++ ランタイム `.so` 対応。Python 3.12 が **NumPy 1.26.4** を import して実行できます（import、配列加算、行列積、`sum`、`mean` を検証済み）。
- **エンドツーエンドのネットワーク:** `virtio-net` + `lwIP` の IPv4 スタックで DHCP、DNS、ICMP、UDP、TCP、socket syscall をサポート — BusyBox `httpd` と、BearSSL による userspace **HTTPS** クライアントまで動きます。
- **実際にスケジューリングする SMP:** QEMU 上で 4 CPU 起動、LAPIC タイマー、リスケジュール IPI、per-CPU run queue、blocking wakeup 経路（pipe / `wait4()` / socket）の検証済み。
- **もちろん DOOM も動きます**（`doomgeneric`）。

## クイックスタート

リファレンスホストは **Ubuntu 24.04（WSL2 含む）** または macOS。ホスト側ビルドは `clang -target x86_64-elf` + `lld` を使うため、専用のクロス GCC は不要です。

```bash
# 1. ビルド依存パッケージのインストール（Ubuntu 22.04 / 24.04 / WSL2）
sudo apt-get update
sudo apt-get install -y clang lld llvm build-essential make python3 \
  xorriso mtools qemu-system-x86 git

# 2. クローン
git clone https://github.com/itoh5588/Orthox-64.git
cd Orthox-64

# 3. カーネル + ユーザーランド + ブート ISO をビルド（orthos.iso が生成される）
make

# 4. QEMU で起動（シリアルコンソールは stdio。Ctrl-A x で終了）
make run
```

詳細な手順、macOS でのビルド、そしてオプションの **OS 上 GCC 4.7.4 ツールチェーン**のビルド（セルフホストカーネルビルドで使うもの）は [INSTALL.jp.md](INSTALL.jp.md)（英語版: [INSTALL.md](INSTALL.md)）を参照してください。

## コンセプトと設計哲学

- **名前の由来:** 「Ortho-」はオーソドックスに由来し、「-x」は Unix の伝統に由来する。Orthox-64 は、正統派で最小主義の Unix 系 OS を目指す。
- **軽量・堅牢 (Lightweight & Robust):** 不要なシステム複雑性を避け、小さく安定したカーネルとユーザーランド基盤に集中する。
- **実用本位の Unix 互換性:** フル POSIX 準拠を目標にするのではなく、BusyBox、GCC、および関連ツールチェーンを動かすために必要な最小限のカーネル機能と libc を実装する。
- **再発明より統合:** すべての部品をゼロから再実装することを美徳とはしない。既存の高品質なオープンソース資産を統合・移植することを、主要なエンジニアリング課題として扱う。

## なぜ GCC 4.7.4 なのか？

osdev 読者なら当然の疑問でしょう。なぜこんなに古いコンパイラでブートストラップするのか？

これは意図的な選択です。開発初期のシステムに C++ は不要だったので、C++ ツールチェーンをブートストラップに持ち込む理由がありませんでした — そして **GCC 4.7.4 は、C コンパイラだけでビルドできる最後の GCC です**（GCC 4.8 以降は、自分自身のコンパイルに動作する C++ ツールチェーンを必要とします）。この「まだ C++ は使わない」という判断が、結果として最もクリーンなブートストラップ経路になりました。まず **C だけでセルフホスティングのループを閉じる**、という道筋です。また、古くて軽い C-only コンパイラは、メモリマネージャやファイルシステムがまだ安定途上にある若いカーネルにとってはるかに扱いやすい存在です — モダンな GCC はこれらのサブシステムをずっと強く叩きます。C++ ランタイム対応（C++ `.so` のロード、TLS）は、基盤が安定した後から追加しました。

## 特徴

- **64ビット ロングモード:** 完全な 64ビット モードで動作。
- **ブートローダー:** モダンな UEFI/BIOS ブートのために [Limine（リマイン）](https://github.com/limine-bootloader/limine) を採用。
- **メモリ管理:** PMM (物理メモリマネージャ) およびページングによる VMM (仮想メモリマネージャ)。
- **マルチタスク:** プリエンプティブ・マルチタスク、カーネルスレッド、および SMP 対応の per-CPU scheduler 基盤。
- **ファイルシステム:** 仮想ファイルシステム (VFS)、Read-Write な xv6fs ベースの root filesystem（xv6-riscv から移植、最大 ~16 GB/ファイルの triple-indirect ブロック拡張を追加）、および bring-up / fallback 用の tar 形式初期ラムディスク。
- **USB サポート:** 基本的な USB スタックとマスストレージクラス (MSC) のサポート。
- **ネットワーク:** `virtio-net` と `lwIP` に基づく IPv4 ネットワーク機能を実装済み。DHCP、DNS、ICMP、UDP、TCP、socket syscall、BusyBox `httpd`、外向き HTTP クライアント、さらに BearSSL ベースの userspace HTTPS クライアントまで実装。
- **SMP:** 4 CPU 起動、LAPIC timer、resched IPI、per-CPU run queue、および pipe / wait / socket の blocking wakeup 経路を確認済み。
- **サウンド:** AC97 / Sound Blaster 16 フォールバックによる PCM 再生とビープ音サポート。
- **ユーザーランド:** 標準互換性のための `musl libc` をベースとした環境。
- **共有ライブラリ（動的リンク）:** musl ベースの動的リンカによる `.so` ファイルの位置独立ロード、`dlopen`/`dlsym`、TLS（スレッドローカルストレージ）、C++ ランタイム `.so`、Python C 拡張 `.so` の import を実装・検証済み。
- **ネイティブカーネルセルフコンパイル:** Orthox-64 は、起動中の OS 内部でネイティブ移植済みの GCC 4.7.4 と Binutils 2.26 を使い、自分自身のカーネルをソースからコンパイルできる。ビルドしたカーネルは正常に起動・動作する。セルフホスティングのビルドループは閉じている。
- **移植アプリ:** `doomgeneric`、`Python 3.12`、Python 上の NumPy などの既存ソフトウェアのポーティング。

## 移植済みユーザーランドコンポーネント

- **musl libc:** `1.2.5`
- **BusyBox (`ash` および基本 applet 群):** `1.27.0.git`
- **GNU Binutils:** `2.26`
- **GCC:** `4.7.4`
- **Python:** `3.12.3`
- **NumPy:** `1.26.4`。Python の共有オブジェクト拡張ロード経由で import と基本的な配列演算を Orthox-64 上で検証済み。
- **doomgeneric:** upstream の版番号がツリー内に記録されていない vendored local port

## ステータス

プロジェクトは現在活発に開発中です。2つの大きなマイルストーンを達成しました。

**共有ライブラリ対応:** Orthox-64 は完全な動的リンクをサポートしました。musl の動的リンカが位置独立 `.so` ファイルをロードし、`dlopen`/`dlsym`、TLS、C++ ランタイムサポート、Python C 拡張の共有オブジェクト import をエンドツーエンドで検証済みです。Python 3.12 から NumPy 1.26.4 を import して利用でき、smoke test では NumPy import、配列加算、行列積、`sum`、`mean` まで確認済みです。

**ネイティブカーネルセルフコンパイル達成（Day 43, 2026-05-03）:** Orthox-64 は、起動中の OS 内部でネイティブ移植済みの GCC 4.7.4 ツールチェーンを使い、自分自身のカーネルをソースからコンパイルできます。ビルドしたカーネルは正常に起動・動作します。OS が自分自身をビルドするというセルフホスティングビルド成功。

SMP 基盤は安定しており、Orthox-64 は QEMU 上で 4 CPU 起動、per-CPU run queue 、pipe / `wait4()` / DNS / socket / UDP echo / BusyBox `httpd` / userspace HTTPS の SMP 実経路確認まで完了しています。今後は、これらの基盤の上でさらなるユーザーランド互換性の拡充と高レベル機能の実装を進めます。

## 書籍

Orthox-64 の設計と実装は、システムコール境界から内側へと OS を読み解く書き下ろしの書籍として文書化されています — ブート、メモリ、スケジューリング、fork/exec、VFS、musl、セルフホスティング、そして締めくくりに AI エージェントと共に OS 機能を設計する章を収録。

- **日本語版:** [Amazon で発売中](https://www.amazon.co.jp/dp/B0H468KNCT)
- **英語版:** 準備中。

## 謝辞

Orthox-64 は、以下のプロジェクトからインスピレーションを受け、実装の参考にしています。

- **[MikanOS](https://github.com/uchan-nos/mikanos)**: [uchan-nos](https://github.com/uchan-nos) 氏による現代的な自作OS。カーネルアーキテクチャや一部のプリミティブなセットアップにおいて、その優れた実装を参考に開発されています。
- **[Limine](https://github.com/limine-bootloader/limine)**: モダンなUEFI/BIOSブートをサポートするためのブートローダーとして採用しています。
- **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)**: MIT PDOS による教育用 OS xv6（MIT ライセンス）。Orthox-64 のルートファイルシステム（xv6fs）は xv6-riscv の `kernel/fs.c`・`bio.c`・`log.c` を移植し、triple-indirect ブロック対応と Orthox-64 カーネル環境への適合を加えたものです。

## ライセンス

Orthox-64 本体は MIT ライセンス（[LICENSE](LICENSE)）で公開しています。

カーネルには xv6-riscv（MIT）から移植したファイルシステムコードが含まれ、`ports/` 配下には musl・lwIP・BearSSL・Limine・CPython・zlib・BusyBox・GNU Make/Binutils/GCC などの第三者コンポーネントを同梱しています。これらの著作権表示・ライセンス（一部は GPL）と配布上の注意点は [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にまとめています。
