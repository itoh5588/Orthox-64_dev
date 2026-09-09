#!/bin/bash
# stage-2: musl を見せた状態で GCC 4.7.4 (aarch64 移植版) を組み直し、
# **libgcc まで作って** $PREFIX に install する。
# ports/build_gcc474_riscv_stage2.sh の aarch64 版。
#
# なぜ要るか:
#   stage-1 は --without-headers --with-newlib --disable-threads で、
#   **libc が無いので libgcc を作っていない** (cross-aarch64 に libgcc.a が無い)。
#   この状態では「コンパイルは出来るがリンクが出来ない」ので、
#   Orthox 上で動く静的バイナリ (= セルフホストの as / ld / cc1) を作れない。
#
#   riscv64 も同じ順序を踏んでいる。build_binutils_orthox.sh:23 と
#   build_gcc474_orthox.sh:26 が stage-2 の産物を明示的に要求している。
#
# 段取り (日報2026-08-01 追記 5 の riscv64 版を aarch64 で真似る):
#   0. これ                              stage-2
#   1. gmp/mpfr/mpc を aarch64 用に組む
#   2. binutils Canadian cross           -> ports/orthox-native-aarch64/
#   3. **as/ld だけで先に確認**          (riscv64 の切り分けを踏襲)
#   4. GCC Canadian cross (cc1)
#   5. busybox ash
#   6. rootfs へ deploy
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
SRC="$PORTS/gcc-4.7.4-aarch64"
BUILD="$SRC/build-aarch64-stage2"
PREFIX="$PORTS/cross-aarch64"
PREREQ="$PORTS/gcc-prereq-host"
SYSROOT="$PORTS/musl-install-aarch64"

JOBS="$(nproc)"

# **組む前に確かめる。** 無いものを組もうとして待つのは無駄
[ -f "$SYSROOT/lib/libc.a" ] || {
  echo "error: $SYSROOT/lib/libc.a が無い" >&2; exit 1; }
[ -d "$SYSROOT/include/sys" ] || {
  echo "error: $SYSROOT/include に musl のヘッダが無い" >&2; exit 1; }
[ -x "$PREFIX/bin/aarch64-linux-musl-as" ] || {
  echo "error: 先に ./build_binutils_aarch64.sh" >&2; exit 1; }
[ -f "$SRC/gcc/config/aarch64/aarch64.c" ] || {
  echo "error: 先に ./port_aarch64_to_gcc47.sh" >&2; exit 1; }

export CFLAGS="-std=gnu89 -w -O1"
export CXXFLAGS="-std=gnu++98 -w -O1"
export LC_ALL=C
export PATH="$PREFIX/bin:$PATH"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# --with-native-system-header-dir=/include が要る。
# 既定では sysroot/usr/include を見るが、**musl は sysroot 直下に include/ を置く**
# (riscv64 の stage-2 と同じ)。
../configure \
  --target=aarch64-linux-musl \
  --prefix="$PREFIX" \
  --with-arch=armv8-a \
  --with-abi=lp64 \
  --with-gmp="$PREREQ" \
  --with-mpfr="$PREREQ" \
  --with-mpc="$PREREQ" \
  --with-sysroot="$SYSROOT" \
  --with-native-system-header-dir=/include \
  --enable-languages=c \
  --disable-multilib \
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
GCC="$PREFIX/bin/aarch64-linux-musl-gcc"
"$GCC" --version | head -1
"$GCC" -v 2>&1 | grep -E "Thread model" | head -1
find "$PREFIX" -name libgcc.a -printf '%p  %s bytes\n'

# **--version で済ませない。** musl と繋いで静的にリンクできることを実際に見る。
# ここが通らないと段取り 2 以降は 1 行も進まない
echo
echo "=== 受け入れ判定: musl と静的リンクする ==="
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cat > "$T/hello.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    char *p = malloc(64);
    strcpy(p, "hello from aarch64 musl");
    printf("%s (argc=%d)\n", p, argc);
    free(p);
    return 0;
}
EOF
"$GCC" -O2 -static -o "$T/hello" "$T/hello.c"
file "$T/hello"
file "$T/hello" | grep -q "ELF 64-bit LSB executable, ARM aarch64" \
  || { echo "★ aarch64 の実行ファイルになっていない" >&2; exit 1; }
file "$T/hello" | grep -q "statically linked" \
  || { echo "★ 静的リンクになっていない (Orthox に動的リンカは無い)" >&2; exit 1; }
"$PREFIX/bin/aarch64-linux-musl-nm" "$T/hello" | grep -q " T _start" \
  || { echo "★ _start が無い (crt1.o が入っていない)" >&2; exit 1; }
echo "静的リンク OK"
