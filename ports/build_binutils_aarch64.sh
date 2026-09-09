#!/bin/bash
# aarch64-linux-musl 向け binutils (as / ld) を組む。
# ports/build_binutils_riscv64.sh の aarch64 版。**target だけが違う。**
#
# riscv64 との違い:
#   riscv64  上流入りが 2.28 (2017-03) と遅く、2.26 では組めなかった
#   aarch64  **2.23 (2012-10) から上流にある。**移植も版の制約も無い
#
# これは「x86_64 Linux で動く、aarch64 のコードを吐く」クロス binutils。
# セルフホスト (Orthox 自身で動く as / ld) は build_binutils_orthox_aarch64.sh
# のほうで、そちらは **この成果物を使って**組む。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$PORTS/cross-aarch64"
VER="${BINUTILS_VER:-2.42}"
TARBALL="binutils-${VER}.tar.xz"
SRC="$PORTS/binutils-${VER}"
BUILD="$SRC/build-aarch64"

JOBS="$(nproc)"

cd "$PORTS"
if [ ! -f "$TARBALL" ]; then
  echo "--- binutils-${VER} を取得"
  curl -fL -O "https://ftp.gnu.org/gnu/binutils/${TARBALL}"
fi
if [ ! -d "$SRC" ]; then
  echo "--- 展開"
  tar -xf "$TARBALL"
fi

# **組む前に確かめる。** 無いものを組もうとして 20 分待つのは無駄
[ -f "$SRC/gas/config/tc-aarch64.c" ] || {
  echo "error: $SRC に aarch64 バックエンドが無い" >&2; exit 1; }

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

../configure \
  --target=aarch64-linux-musl \
  --prefix="$PREFIX" \
  --disable-nls \
  --disable-werror \
  --disable-multilib \
  --with-sysroot="$PREFIX/aarch64-linux-musl"

make -j"$JOBS"
make install

echo
echo "=== 完了 ==="
"$PREFIX/bin/aarch64-linux-musl-as" --version | head -1
"$PREFIX/bin/aarch64-linux-musl-ld" --version | head -1
