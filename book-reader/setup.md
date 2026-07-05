# Orthox-64 セットアップガイド（読者向け）

このガイドは本書『Orthox-64』を読み進めるための**最短経路**を案内します。Linux（Ubuntu 24.04 + WSL2）を主軸に説明します。macOS の場合は補足を参照してください。

## 所要時間の目安

- パッケージインストール: 5〜10 分（回線次第）
- 初回ビルド: 3〜10 分（マシン性能次第）
- QEMU 起動確認: 1 分

## 前提

- Windows をお使いの方は **WSL2 + Ubuntu 24.04** をセットアップしてください（Microsoft Store から Ubuntu をインストールし、`wsl --update` で WSL2 を最新化）。
- 本ガイドのコマンドは WSL2 内の Ubuntu シェル、または Linux ネイティブのシェルで実行します。
- ディスク空き容量は 5 GB 以上を推奨します（ISO・rootfs・ビルド成果物のため）。

## ステップ 1: 必要パッケージのインストール

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

これだけで揃います。`x86_64-elf-gcc` のようなクロスツールチェインの自前ビルドは**不要**です（`clang` が `-target x86_64-elf` でクロスコンパイルを担います）。

## ステップ 2: ソースを取得してビルド

```bash
git clone https://github.com/yourusername/Orthox-64.git
cd Orthox-64
make
```

`make` 一発で `kernel.elf`、ユーザーランド ELF、`orthos.iso`（Limine 起動 ISO）まで作られます。

## ステップ 3: QEMU で起動確認

```bash
make run
```

シリアル出力がターミナルに流れ、最終的に `/bin/ash` のプロンプトが出れば成功です。QEMU を抜けるには **`Ctrl-A` を押してから `x`** を続けて押します。

## 動いたら次は

このフォルダ（`book-reader/`）に戻り、AI 家庭教師を起動してください。

```bash
cd book-reader
claude
```

最初の話題として「セットアップは終わりました。本書 Ch○○ を読みたい」など伝えれば、AI が章の写経・実装支援を始めます。

## トラブル Q&A

### `make` でリンクエラー（`ld: cannot find -lc` など）

musl の sysroot がまだ整っていない可能性があります。`make toolchain` を一度走らせてから `make` をやり直してください。

### `qemu-system-x86_64: not found`

```bash
sudo apt-get install -y qemu-system-x86
```

### WSL2 で `make run` 後、画面がブラックアウトしたまま戻れない

`Ctrl-A` → `x` で QEMU を強制終了できます。WSL2 のターミナル端末側で `reset` を打てば表示が戻ります。

### WSL2 + WSLg で音が鳴らない（DOOM デモなど）

QEMU の AC97 経路は `tests/run_qemu_ac97.sh` 経由で `/mnt/wslg/PulseServer` を自動検出する作りになっています。`make ac97run` を使ってください。

### ビルドが極端に遅い・OOM Killer に殺される

WSL2 のメモリ上限を `.wslconfig` で 8 GB 程度に上げてください（Windows 側 `%USERPROFILE%\.wslconfig`）。

```ini
[wsl2]
memory=8GB
swap=4GB
```

## macOS の場合

```bash
brew install llvm lld make python3 xorriso mtools qemu git
git clone https://github.com/yourusername/Orthox-64.git
cd Orthox-64
make
make run
```

Apple Silicon でも動きますが、Rosetta は不要です（QEMU が x86_64 をエミュレーションします）。

## 確認: AI 家庭教師に環境チェックを頼む

```bash
cd book-reader
claude
```

起動後、AI に「`/env-check` を実行して」と頼めば、必要ツールの存在確認を AI 側で診断してくれます。
