#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_DIR="$ROOT/ports/python-install"
LIB_DIR="$INSTALL_DIR/lib/python3.12"
ROOTFS_LIB="$ROOT/rootfs/lib"
ROOTFS_BIN="$ROOT/rootfs/bin"

if [ ! -d "$LIB_DIR" ]; then
    echo "Error: Python standard library not found at $LIB_DIR"
    exit 1
fi

mkdir -p "$ROOTFS_LIB"
mkdir -p "$ROOTFS_BIN"

echo "Creating python312.zip..."
cd "$LIB_DIR"

# 不要なファイルやディレクトリを除外しつつ zip を作成する
rm -f "$ROOTFS_LIB/python312.zip"
zip -r -0 "$ROOTFS_LIB/python312.zip" . -x "*__pycache__*" "*/test/*" "test/*" "*.so" "*.pyc" "*.pyo" "idlelib/*" "tkinter/*" "turtledemo/*"

echo "Copying python binary and compiling wrapper..."
# 実際のバイナリは python3.12 として配置
cp "$INSTALL_DIR/bin/python3.12" "$ROOTFS_BIN/python3.12"
chmod +x "$ROOTFS_BIN/python3.12"

# C言語で書かれたラッパーをコンパイルして配置する
cd "$ROOT"
./ports/orthos-musl-gcc.sh ports/python3_wrapper.c -static -fno-PIC -fno-PIE -o "$ROOTFS_BIN/python3"
chmod +x "$ROOTFS_BIN/python3"

echo "Done! python312.zip and python3 wrapper are ready in rootfs."
