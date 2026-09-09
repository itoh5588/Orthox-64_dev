#!/bin/bash
# binutils (as / ld / ar など) を **Orthox aarch64 上で動く形**で組む。
# ports/build_binutils_orthox.sh の aarch64 版 (riscv64 が host/target だったもの)。
#
# ここまでのビルドは「x86_64 Linux で動く aarch64 向けクロス」だったが、
# セルフホストには「aarch64 Orthox 自身で動く」ものが要る。
#   build  = x86_64-unknown-linux-gnu   (組む機械)
#   host   = aarch64-linux-musl         (動かす機械 = Orthox)
#   target = aarch64-linux-musl         (吐くコード)
#
# **Orthox には動的リンカが無いので -static 必須。**
#
# **--disable-gprofng が riscv64 版との差。** gprofng (binutils 2.39 で入った
# プロファイラ) は glibc 固有の struct sigevent._sigev_un を使うので musl で
# 落ちる:
#   gprofng/libcollector/dispatcher.c:602:
#     error: 'struct sigevent' has no member named '_sigev_un'
# **riscv64 が踏まなかったのは gprofng が riscv64 を対象にしていないから。**
# aarch64 は対象なので出る。セルフホストに要るのは as/ld/ar/objcopy/objdump/nm
# だけなので落として構わない。
#
# 前提: build_gcc474_aarch64_stage2.sh (libgcc と musl sysroot が要る)
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
VER="${BINUTILS_VER:-2.42}"
SRC="$PORTS/binutils-${VER}"
BUILD="$SRC/build-orthox-aarch64"
PREFIX="$PORTS/orthox-native-aarch64"   # Orthox の / に相当する置き場
CROSS="$PORTS/cross-aarch64/bin/aarch64-linux-musl"

JOBS="$(nproc)"

# **riscv64 の成果物を絶対に踏まない。** ports/orthox-native は riscv64 の 50MB
case "$PREFIX" in
  */orthox-native) echo "error: riscv64 の置き場を指している" >&2; exit 1;;
esac

[ -d "$SRC" ] || { echo "error: $SRC が無い。先に ./build_binutils_aarch64.sh" >&2; exit 1; }
[ -x "${CROSS}-gcc" ] || { echo "error: ${CROSS}-gcc が無い" >&2; exit 1; }
# **libgcc が無いとリンクできない。** stage-1 のままだとここで気づける
[ -n "$(find "$PORTS/cross-aarch64" -name libgcc.a -print -quit)" ] || {
  echo "error: libgcc.a が無い。先に ./build_gcc474_aarch64_stage2.sh" >&2; exit 1; }

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# Orthox 上では /usr/bin に置く前提で prefix を切る (実行時のパス解決に効く)
../configure \
  --build=x86_64-unknown-linux-gnu \
  --host=aarch64-linux-musl \
  --target=aarch64-linux-musl \
  --prefix=/usr \
  --disable-nls \
  --disable-werror \
  --disable-multilib \
  --disable-plugins \
  --disable-gprofng \
  --disable-shared \
  --enable-static \
  CC="${CROSS}-gcc" \
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
ls "$PREFIX/usr/bin/" | tr '\n' ' '; echo

# ---- 配線証明 -------------------------------------------------------------
# **--version で済ませない。** ここで動かせるのは file / readelf までだが、
# 「aarch64 の静的 ELF になっていること」だけは必ず見る。
# 実際に動かす確認は段取り 3 (Orthox 上で as/ld を叩く) でやる
for t in as ld ar ranlib objcopy objdump nm strip; do
  f="$PREFIX/usr/bin/$t"
  [ -f "$f" ] || { echo "★ $t が入っていない" >&2; exit 1; }
  file "$f" | grep -q "ELF 64-bit LSB executable, ARM aarch64" \
    || { echo "★ $t が aarch64 の実行ファイルでない: $(file -b "$f")" >&2; exit 1; }
  file "$f" | grep -q "statically linked" \
    || { echo "★ $t が静的でない (Orthox に動的リンカは無い): $(file -b "$f")" >&2; exit 1; }
done
echo "  8 本とも aarch64 の静的 ELF  ok"
file "$PREFIX/usr/bin/as"
du -sh "$PREFIX"

echo
echo "riscv64 の ports/orthox-native は無傷:"
du -sh "$PORTS/orthox-native" 2>/dev/null || echo "  (無い)"
