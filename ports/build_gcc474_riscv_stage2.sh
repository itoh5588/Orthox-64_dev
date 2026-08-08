#!/bin/bash
# stage-2: musl を見せた状態で GCC 4.7.4 (移植版) を組み直し、$PREFIX に install する。
#
# stage-1 (build_gcc474_riscv.sh) は --without-headers --disable-threads だった。
# libc が無いのでスレッドモデルが single になり、libgcc も一部しか作れない。
# musl が出来たので、それを sysroot として見せて posix スレッドで組み直す。
#
# 手順:
#   1. build_binutils_riscv64.sh
#   2. port_riscv_to_gcc47.sh
#   3. build_gcc474_riscv.sh          (stage-1)
#   4. build_musl.sh                  (stage-1 の gcc で musl を作る)
#   5. これ                            (stage-2)
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
SRC="$PORTS/gcc-4.7.4-riscv"
BUILD="$SRC/build-riscv64-stage2"
PREFIX="$PORTS/cross-riscv64"
PREREQ="$PORTS/gcc-prereq-host"
SYSROOT="$PORTS/musl-install-riscv64"

JOBS="$(nproc)"

[ -f "$SYSROOT/lib/libc.a" ] || { echo "error: musl が無い。先に musl を組むこと"; exit 1; }
[ -x "$PREFIX/bin/riscv64-linux-musl-as" ] || { echo "error: 先に ./build_binutils_riscv64.sh"; exit 1; }

export CFLAGS="-std=gnu89 -w -O1"
export CXXFLAGS="-std=gnu++98 -w -O1"
export LC_ALL=C
export PATH="$PREFIX/bin:$PATH"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# --with-native-system-header-dir=/include が要る。
# 既定では sysroot/usr/include を見るが、musl は sysroot 直下に include/ を置く。
#
# multilib は lp64d / lp64 の 2 本のまま。musl は lp64d しか作っていないが、
# libgcc の大半は libc を必要としないので lp64 側も作れる。
../configure \
  --target=riscv64-linux-musl \
  --prefix="$PREFIX" \
  --with-arch=rv64gc \
  --with-abi=lp64d \
  --with-gmp="$PREREQ" \
  --with-mpfr="$PREREQ" \
  --with-mpc="$PREREQ" \
  --with-sysroot="$SYSROOT" \
  --with-native-system-header-dir=/include \
  --enable-languages=c \
  --enable-multilib \
  --enable-threads=posix \
  --disable-nls \
  --disable-werror \
  --disable-shared \
  --disable-libssp \
  --disable-libgomp \
  --disable-libmudflap \
  --disable-libquadmath \
  --disable-libatomic \
  MAKEINFO=missing

echo "=== configure 完了。all-gcc を開始 ==="
make -j"$JOBS" all-gcc
echo "=== all-target-libgcc を開始 ==="
make -j"$JOBS" all-target-libgcc
echo "=== install ==="
make install-gcc install-target-libgcc

echo
echo "=== 完了 ==="
"$PREFIX/bin/riscv64-linux-musl-gcc" --version | head -1
"$PREFIX/bin/riscv64-linux-musl-gcc" -v 2>&1 | grep -E "Thread model|Configured" | head -2
