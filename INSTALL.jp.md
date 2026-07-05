# Orthox-64 インストールガイド

## 必要条件

Orthox-64 をビルド・実行するには、ホスト側に以下のツールが必要です。

- **ホスト OS:** Linux（リファレンス環境は Ubuntu 24.04 + WSL2）または macOS
- **C/C++ コンパイラ:** `clang`（`-target x86_64-elf` でクロスコンパイル。`x86_64-elf-gcc` 等の専用ツールチェインは不要）
- **リンカ:** `lld`
- **ビルドツール:** `make`、`python3`（**3.10 以降必須** — rootfs 生成スクリプトが新しい型ヒント構文を使うため、macOS 標準の python3 3.9 では不足）
- **イメージ作成:** `xorriso`、`mtools`
- **エミュレータ:** `qemu-system-x86_64`

### Ubuntu 22.04 / 24.04（WSL2 含む）

```bash
sudo apt-get update
sudo apt-get install -y \
  clang lld llvm \
  build-essential \
  make python3 \
  xorriso mtools \
  qemu-system-x86 \
  git
```

### macOS

```bash
brew install llvm lld make python3 xorriso mtools qemu git
```

`brew` の `clang` と `python3` をシステム標準より優先するよう `PATH` を調整してください（macOS 標準の Python は 3.9 で、rootfs 生成スクリプトには古すぎます）。例: `export PATH="/opt/homebrew/bin:$PATH"`

## ビルド手順

1. **リポジトリのクローン**
   ```bash
   git clone https://github.com/yourusername/Orthox-64.git
   cd Orthox-64
   ```

2. **カーネル・ユーザーランド・ブート ISO を一括ビルド**
   ```bash
   make
   ```
   `orthos.iso`（Limine ブート対応）と `kernel.elf`、各種ユーザーランドバイナリが生成されます。

## 実行方法

```bash
make run
```

内部的には `tests/run_qemu_stdio.sh` が呼ばれ、`qemu-system-x86_64` でシリアルコンソールを stdio に接続して `orthos.iso` を起動します（`-serial mon:stdio`）。

QEMU を抜けるには `Ctrl-A x` を入力してください。

## 補足: OS 内ネイティブツールチェイン

`ports/build_gcc.sh` と `ports/build_binutils.sh` は、Orthox-64 の**内側で動く** GCC 4.7.4 / Binutils 2.26 を構築するためのスクリプトです（Day 43 で達成した「OS による自分のカーネルのビルド」で使用）。macOS の開発ホストを想定しており、上記の標準ホスト側クロスビルドには不要です。
