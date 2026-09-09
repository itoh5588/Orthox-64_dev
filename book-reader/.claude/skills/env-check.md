---
name: env-check
description: Orthox-64 のビルド・実行に必要なホスト側ツール群が揃っているかを診断し、不足があれば apt/brew コマンドで補う案を提示する。読者が「環境チェック」「セットアップ確認」「make が通らない」と言ったときに使用。
---

# /env-check — Orthox-64 ホスト環境診断

読者がリポジトリを clone した直後、または `make` で詰まったときに使うスキルです。**読者の環境を診断し、不足を具体的に教える**ことが目的です。

## 診断手順

以下のチェックを順に実施してください。すべて `Bash` ツールで実行できます。

### 1. ホスト OS の判定

```sh
uname -a
cat /etc/os-release 2>/dev/null | head -5
[ -d /mnt/wslg ] && echo "WSL2 detected"
```

WSL2 ホストなら `WSL2 detected` が出ます。判定結果でこの後の案内（apt / brew）を切り替えてください。

### 2. 必須ツールの存在確認

```sh
for t in clang lld llvm-ar make python3 qemu-system-x86_64 xorriso mtools git; do
  printf "%-20s " "$t:"
  command -v "$t" 2>&1 || echo "(MISSING)"
done
```

各ツールについて、見つかれば**パス**、見つからなければ `(MISSING)` を表示します。

### 3. バージョン確認（任意、参考情報）

```sh
clang --version | head -1
qemu-system-x86_64 --version | head -1
make --version | head -1
python3 --version
```

特に `clang` は `-target x86_64-elf` をサポートするバージョンが必要ですが、Ubuntu 22.04 以降の `clang` であれば問題ありません。

### 4. リポジトリ位置の確認

```sh
ls -d ../kernel ../include ../Makefile 2>/dev/null
```

`book-reader/` の親に Orthox-64 のソースがあることを確認します。無ければ読者はディレクトリを間違えています。

### 5. ディスク空き容量

```sh
df -h .
```

5 GB 以上空いていることを確認してください（ISO・rootfs・ビルド成果物用）。

## 不足ツールの補い方を提案

不足がある場合、ホスト OS に応じて以下のいずれかを読者に提案してください。

### Ubuntu / WSL2

```sh
sudo apt-get update
sudo apt-get install -y clang lld llvm build-essential make python3 xorriso mtools qemu-system-x86 git
```

部分的に欠けている場合は、必要なものだけ並べてください（例: `sudo apt-get install -y qemu-system-x86 xorriso`）。

### macOS

```sh
brew install llvm lld make python3 xorriso mtools qemu git
```

## 報告フォーマット

診断結果は以下の形でまとめると読者が理解しやすいです。

```text
== Orthox-64 環境チェック ==
ホスト    : Ubuntu 24.04 (WSL2)
リポジトリ: /home/.../Orthox-64 ✓
ディスク  : 12G 空き ✓

必須ツール:
  clang             ✓ (18.1.3)
  lld               ✓
  qemu-system-x86_64 ✗ (未インストール)
  ...

不足: qemu-system-x86_64
対処: sudo apt-get install -y qemu-system-x86
```

## 注意

- 診断系コマンドはすべて読み取り専用です。`apt-get install` などの**書き込み系は提案までに留め、実行は読者に許可を求めてください**。
- WSL2 か Linux ネイティブかで `.wslconfig` の助言要否が変わります。WSL2 でメモリ問題があれば `book-reader/setup.md` の該当 Q&A を案内してください。
