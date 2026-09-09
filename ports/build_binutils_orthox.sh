#!/bin/bash
# binutils (as / ld / ar など) を **Orthox 上で動く形**で組む。
#
# ここまでのビルドは全部「x86_64 Linux で動く riscv64 向けクロス」だったが、
# セルフホストには「riscv64 Orthox 自身で動く」ものが要る。
#   build  = x86_64-unknown-linux-gnu   (組む機械)
#   host   = riscv64-linux-musl         (動かす機械 = Orthox)
#   target = riscv64-linux-musl         (吐くコード)
#
# Orthox には動的リンカが無いので **-static 必須**。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
VER="${BINUTILS_VER:-2.42}"
SRC="$PORTS/binutils-${VER}"
BUILD="$SRC/build-orthox"
PREFIX="$PORTS/orthox-native"        # Orthox の / に相当する置き場
CROSS="$PORTS/cross-riscv64/bin/riscv64-linux-musl"

JOBS="$(nproc)"

[ -d "$SRC" ] || { echo "error: $SRC が無い。先に ./build_binutils_riscv64.sh"; exit 1; }
[ -x "${CROSS}-gcc" ] || { echo "error: ${CROSS}-gcc が無い。先に ./build_gcc474_riscv_stage2.sh"; exit 1; }

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# Orthox 上では /usr/bin に置く前提で prefix を切る (実行時のパス解決に効く)
../configure \
  --build=x86_64-unknown-linux-gnu \
  --host=riscv64-linux-musl \
  --target=riscv64-linux-musl \
  --prefix=/usr \
  --disable-nls \
  --disable-werror \
  --disable-multilib \
  --disable-plugins \
  --disable-shared \
  --enable-static \
  CC="${CROSS}-gcc" \
  CXX="${CROSS}-g++" \
  AR="${CROSS}-ar" \
  RANLIB="${CROSS}-ranlib" \
  CFLAGS="-O2 -static" \
  LDFLAGS="-static" \
  CC_FOR_BUILD="gcc" \
  MAKEINFO=missing

make -j"$JOBS"
rm -rf "$PREFIX"
make install DESTDIR="$PREFIX"

echo
echo "=== 完了 ==="
file "$PREFIX/usr/bin/as" 2>/dev/null || true
ls -la "$PREFIX/usr/bin/" | head
