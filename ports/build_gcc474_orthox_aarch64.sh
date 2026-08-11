#!/bin/bash
# GCC 4.7.4 (aarch64 移植版) を **Orthox 上で動く形**で組む。セルフホストの本体。
# ports/build_gcc474_orthox.sh の aarch64 版。
#
#   build  = x86_64-unknown-linux-gnu   (組む機械)
#   host   = aarch64-linux-musl         (動かす機械 = Orthox)
#   target = aarch64-linux-musl         (吐くコード)
#
# host が aarch64 なので gmp/mpfr/mpc も aarch64 用に組み直す必要がある
# (ports/gcc-prereq-host は x86_64 用、gcc-prereq-riscv64 は riscv64 用)。
#
# Orthox には動的リンカが無いので -static 必須。
#
# 前提:
#   ./build_gcc474_aarch64_stage2.sh      libgcc と musl sysroot
#   ./build_binutils_orthox_aarch64.sh    Orthox 上の as/ld
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
SRC="$PORTS/gcc-4.7.4-aarch64"
BUILD="$SRC/build-orthox-aarch64"
PREFIX="$PORTS/orthox-native-aarch64"   # binutils と同じツリーに入れる
PREREQ="$PORTS/gcc-prereq-aarch64"      # aarch64 で動く gmp/mpfr/mpc
CROSS="$PORTS/cross-aarch64/bin/aarch64-linux-musl"
MUSL="$PORTS/musl-install-aarch64"
TRIPLE=aarch64-linux-musl

GMP_VER=6.1.0
MPFR_VER=3.1.4
MPC_VER=1.0.3
JOBS="$(nproc)"

# **riscv64 の成果物を絶対に踏まない**
case "$PREFIX" in */orthox-native) echo "error: riscv64 の置き場" >&2; exit 1;; esac
case "$PREREQ" in */gcc-prereq-riscv64|*/gcc-prereq-host)
  echo "error: 他アーキの置き場" >&2; exit 1;; esac

[ -x "${CROSS}-gcc" ] || { echo "error: ${CROSS}-gcc が無い" >&2; exit 1; }
[ -n "$(find "$PORTS/cross-aarch64" -name libgcc.a -print -quit)" ] || {
  echo "error: libgcc.a が無い。先に ./build_gcc474_aarch64_stage2.sh" >&2; exit 1; }
[ -x "$PREFIX/usr/bin/as" ] || {
  echo "error: 先に ./build_binutils_orthox_aarch64.sh" >&2; exit 1; }
[ -f "$MUSL/lib/libc.a" ] || { echo "error: $MUSL/lib/libc.a が無い" >&2; exit 1; }

# --- 前提ライブラリを aarch64 用に組む -----------------------------------
build_prereq() {
  local name="$1" ver="$2" tarball="$3"
  shift 3
  if [ -f "$PREREQ/lib/lib${name}.a" ]; then
    echo "--- ${name}-${ver} (aarch64): 既にある。skip"
    return
  fi
  echo "--- ${name}-${ver} を aarch64 用に組む"
  local work="$PORTS/.prereq-aarch64"
  mkdir -p "$work"
  cd "$work"
  rm -rf "${name}-${ver}" "build-${name}"
  tar -xf "$PORTS/$tarball"
  mkdir -p "build-${name}"
  cd "build-${name}"
  "../${name}-${ver}/configure" \
    --build=x86_64-unknown-linux-gnu \
    --host="$TRIPLE" \
    --prefix="$PREREQ" \
    --disable-shared --enable-static \
    CC="${CROSS}-gcc" CFLAGS="-O2" \
    AR="${CROSS}-ar" RANLIB="${CROSS}-ranlib" "$@"
  make -j"$JOBS"
  make install
  cd "$PORTS"
}

build_prereq gmp  "$GMP_VER"  "gmp-${GMP_VER}.tar.bz2"
build_prereq mpfr "$MPFR_VER" "mpfr-${MPFR_VER}.tar.bz2" --with-gmp="$PREREQ"
build_prereq mpc  "$MPC_VER"  "mpc-${MPC_VER}.tar.gz"    --with-gmp="$PREREQ" --with-mpfr="$PREREQ"

# --- GCC 本体 -------------------------------------------------------------
export LC_ALL=C
# GCC のサブディレクトリ (libdecnumber / libiberty / libcpp) は
# host 用の ar/ranlib を **修飾なしの名前** aarch64-linux-musl-ar で呼ぶので、
# PATH を通しておかないと Error 127 で落ちる (riscv64 版のコメントより)。
export PATH="$PORTS/cross-aarch64/bin:$PATH"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

../configure \
  --build=x86_64-unknown-linux-gnu \
  --host="$TRIPLE" \
  --target="$TRIPLE" \
  --prefix=/usr \
  --with-arch=armv8-a \
  --with-abi=lp64 \
  --with-gmp="$PREREQ" \
  --with-mpfr="$PREREQ" \
  --with-mpc="$PREREQ" \
  --with-sysroot=/ \
  --with-native-system-header-dir=/include \
  --with-build-sysroot="$MUSL" \
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
  --disable-bootstrap \
  CC="${CROSS}-gcc" \
  CFLAGS="-std=gnu89 -w -O1 -static" \
  LDFLAGS="-static" \
  CC_FOR_BUILD="gcc" \
  CFLAGS_FOR_BUILD="-std=gnu89 -w -O1" \
  MAKEINFO=missing

echo "=== configure 完了。all-gcc を開始 ==="
make -j"$JOBS" all-gcc
echo "=== install ==="
make install-gcc DESTDIR="$PREFIX"

# --- Orthox 上でコンパイルできるように周辺を揃える -----------------------
# gcc は --with-sysroot=/ --with-native-system-header-dir=/include なので、
# Orthox の / から見て /include に musl のヘッダ、/lib に crt と libc が要る。
# libgcc はクロス側 (stage-2) で作ったものをそのまま置く
# (ターゲットが同じ aarch64-linux-musl なので中身は同一)。
echo "=== musl と libgcc を Orthox ツリーに配置 ==="
mkdir -p "$PREFIX/include" "$PREFIX/lib" "$PREFIX/usr/lib/gcc/$TRIPLE/4.7.4"
cp -a "$MUSL/include/." "$PREFIX/include/"
cp -a "$MUSL/lib/." "$PREFIX/lib/"
# libgcc.a だけでなく crtbegin*/crtend* も要る (ld が crtbeginT.o を探す)
GCCLIB="$PORTS/cross-aarch64/lib/gcc/$TRIPLE/4.7.4"
cp -a "$GCCLIB/libgcc.a" "$GCCLIB"/crt*.o "$PREFIX/usr/lib/gcc/$TRIPLE/4.7.4/"

# man/info は Orthox では読めないうえ 4MB 近くあるので落とす
rm -rf "$PREFIX/usr/share/man" "$PREFIX/usr/share/info"

# ---- 配線証明 -------------------------------------------------------------
# **--version で済ませない。** ここで見られるのは ELF の形までだが、
# 「aarch64 の静的 ELF になっていること」は必ず見る
echo
echo "=== 受け入れ判定 ==="
CC1="$(find "$PREFIX/usr" -name cc1 -print -quit)"
[ -n "$CC1" ] || { echo "★ cc1 が入っていない" >&2; exit 1; }
for f in "$CC1" "$PREFIX/usr/bin/gcc" "$PREFIX/usr/bin/cpp"; do
  [ -f "$f" ] || { echo "★ $f が無い" >&2; exit 1; }
  file "$f" | grep -q "ELF 64-bit LSB executable, ARM aarch64" \
    || { echo "★ $f が aarch64 でない: $(file -b "$f")" >&2; exit 1; }
  file "$f" | grep -q "statically linked" \
    || { echo "★ $f が静的でない: $(file -b "$f")" >&2; exit 1; }
done
echo "  cc1 / gcc / cpp とも aarch64 の静的 ELF  ok"

# **Orthox 上の gcc が探す場所に物が在ること。** ここが欠けると
# 「gcc は起動するがヘッダが無い」で OS の中まで行ってから気づく
for p in "$PREFIX/include/stdio.h" "$PREFIX/lib/libc.a" "$PREFIX/lib/crt1.o" \
         "$PREFIX/usr/lib/gcc/$TRIPLE/4.7.4/libgcc.a"; do
  [ -f "$p" ] || { echo "★ $p が無い" >&2; exit 1; }
done
echo "  /include /lib /usr/lib/gcc の中身  ok"

echo
echo "=== 完了 ==="
ls -la "$CC1"
du -sh "$PREFIX"
echo "riscv64 の ports/orthox-native は無傷:"
du -sh "$PORTS/orthox-native" 2>/dev/null || echo "  (無い)"
