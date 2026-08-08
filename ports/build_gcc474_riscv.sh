#!/bin/bash
# 前方移植した GCC 4.7.4 + RISC-V バックエンドを riscv64-linux-musl 向けに組む。
# 先に ./build_binutils_riscv64.sh と ./port_riscv_to_gcc47.sh を通すこと。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
SRC="$PORTS/gcc-4.7.4-riscv"
BUILD="$SRC/build-riscv64"
PREFIX="$PORTS/cross-riscv64"
PREREQ="$PORTS/gcc-prereq-host"

JOBS="$(nproc)"

[ -d "$SRC/gcc/config/riscv" ] || { echo "error: 先に ./port_riscv_to_gcc47.sh"; exit 1; }
[ -x "$PREFIX/bin/riscv64-linux-musl-as" ] || { echo "error: 先に ./build_binutils_riscv64.sh"; exit 1; }
[ -f "$PREREQ/lib/libmpc.a" ] || { echo "error: 先に ./build_gcc464_riscv.sh (前提ライブラリを作る)"; exit 1; }

# 4.7.4 も C89 時代のコード。gcc 13 の既定では通らない。
export CFLAGS="-std=gnu89 -w -O1"
export CXXFLAGS="-std=gnu++98 -w -O1"
export LC_ALL=C
export PATH="$PREFIX/bin:$PATH"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

../configure \
  --target=riscv64-linux-musl \
  --prefix="$PREFIX" \
  --with-arch=rv64gc \
  --with-abi=lp64d \
  --with-gmp="$PREREQ" \
  --with-mpfr="$PREREQ" \
  --with-mpc="$PREREQ" \
  --enable-languages=c \
  --enable-multilib \
  --disable-threads \
  --disable-nls \
  --disable-werror \
  --disable-shared \
  --disable-libssp \
  --disable-libgomp \
  --disable-libmudflap \
  --disable-libquadmath \
  --disable-libatomic \
  --without-headers \
  --with-newlib \
  MAKEINFO=missing

echo "=== configure 完了。all-gcc を開始 ==="
make -j"$JOBS" all-gcc
echo "=== all-target-libgcc を開始 ==="
make -j"$JOBS" all-target-libgcc

echo
echo "=== 完了 ==="
echo "cc1:    $BUILD/gcc/cc1"
echo "driver: $BUILD/gcc/xgcc"
find "$BUILD" -name libgcc.a | sed 's/^/libgcc: /'
