#!/bin/bash
# riscv64-linux-musl 向け binutils (as / ld) を組む。
#
# GCC 4.6.4 フォークの検証にはこれが先に要る:
# アセンブラが無いと GCC の configure が TLS 対応を検出できず (gcc_cv_as='' →
# HAVE_AS_TLS が undef)、__thread が emutls に落ちる。musl は native TLS を使うので
# それでは本番にならない。
#
# 2.26 (ports/binutils-2.26) には RISC-V バックエンドが無い。上流入りは 2.28 (2017-03)。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$PORTS/cross-riscv64"
VER="${BINUTILS_VER:-2.42}"
TARBALL="binutils-${VER}.tar.xz"
SRC="$PORTS/binutils-${VER}"
BUILD="$SRC/build-riscv64"

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

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

../configure \
  --target=riscv64-linux-musl \
  --prefix="$PREFIX" \
  --disable-nls \
  --disable-werror \
  --disable-multilib \
  --with-sysroot="$PREFIX/riscv64-linux-musl"

make -j"$JOBS"
make install

echo
echo "=== 完了 ==="
"$PREFIX/bin/riscv64-linux-musl-as" --version | head -1
"$PREFIX/bin/riscv64-linux-musl-ld" --version | head -1
