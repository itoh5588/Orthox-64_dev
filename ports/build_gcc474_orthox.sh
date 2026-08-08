#!/bin/bash
# GCC 4.7.4 (移植版) を **Orthox 上で動く形**で組む。セルフホストの本体。
#
#   build  = x86_64-unknown-linux-gnu   (組む機械)
#   host   = riscv64-linux-musl         (動かす機械 = Orthox)
#   target = riscv64-linux-musl         (吐くコード)
#
# host が riscv64 なので gmp/mpfr/mpc も riscv64 用に組み直す必要がある
# (ports/gcc-prereq-host は x86_64 用なので使えない)。
#
# Orthox には動的リンカが無いので -static 必須。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
SRC="$PORTS/gcc-4.7.4-riscv"
BUILD="$SRC/build-orthox"
PREFIX="$PORTS/orthox-native"           # binutils と同じツリーに入れる
PREREQ="$PORTS/gcc-prereq-riscv64"      # riscv64 で動く gmp/mpfr/mpc
CROSS="$PORTS/cross-riscv64/bin/riscv64-linux-musl"

GMP_VER=6.1.0
MPFR_VER=3.1.4
MPC_VER=1.0.3
JOBS="$(nproc)"

[ -x "${CROSS}-gcc" ] || { echo "error: ${CROSS}-gcc が無い。先に stage-2 を通すこと"; exit 1; }
[ -x "$PREFIX/usr/bin/as" ] || { echo "error: 先に ./build_binutils_orthox.sh"; exit 1; }

# --- 前提ライブラリを riscv64 用に組む -----------------------------------
build_prereq() {
  local name="$1" ver="$2" tarball="$3"
  shift 3
  if [ -f "$PREREQ/lib/lib${name}.a" ]; then
    echo "--- ${name}-${ver} (riscv64): 既にある。skip"
    return
  fi
  echo "--- ${name}-${ver} を riscv64 用に組む"
  local work="$PORTS/.prereq-riscv64"
  mkdir -p "$work"
  cd "$work"
  rm -rf "${name}-${ver}" "build-${name}"
  tar -xf "$PORTS/$tarball"
  mkdir -p "build-${name}"
  cd "build-${name}"
  "../${name}-${ver}/configure" \
    --build=x86_64-unknown-linux-gnu \
    --host=riscv64-linux-musl \
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
# host 用の ar/ranlib を **修飾なしの名前** riscv64-linux-musl-ar で呼ぶので、
# PATH を通しておかないと Error 127 で落ちる。
export PATH="$PORTS/cross-riscv64/bin:$PATH"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

../configure \
  --build=x86_64-unknown-linux-gnu \
  --host=riscv64-linux-musl \
  --target=riscv64-linux-musl \
  --prefix=/usr \
  --with-arch=rv64gc \
  --with-abi=lp64d \
  --with-gmp="$PREREQ" \
  --with-mpfr="$PREREQ" \
  --with-mpc="$PREREQ" \
  --with-sysroot=/ \
  --with-native-system-header-dir=/include \
  --with-build-sysroot="$PORTS/musl-install-riscv64" \
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
# (ターゲットが同じ riscv64-linux-musl なので中身は同一)。
echo "=== musl と libgcc を Orthox ツリーに配置 ==="
MUSL="$PORTS/musl-install-riscv64"
mkdir -p "$PREFIX/include" "$PREFIX/lib" \
         "$PREFIX/usr/lib/gcc/riscv64-linux-musl/4.7.4"
cp -a "$MUSL/include/." "$PREFIX/include/"
cp -a "$MUSL/lib/." "$PREFIX/lib/"
# libgcc.a だけでなく crtbegin*/crtend* も要る (ld が crtbeginT.o を探す)。
# Orthox 側 gcc は --disable-multilib なので lp64d 既定の最上位のものを使う。
GCCLIB="$PORTS/cross-riscv64/lib/gcc/riscv64-linux-musl/4.7.4"
cp -a "$GCCLIB/libgcc.a" "$GCCLIB"/crt*.o \
      "$PREFIX/usr/lib/gcc/riscv64-linux-musl/4.7.4/"

# man/info は Orthox では読めないうえ 4MB 近くあるので落とす
rm -rf "$PREFIX/usr/share/man" "$PREFIX/usr/share/info"

echo
echo "=== 完了 ==="
ls -la "$PREFIX/usr/bin/" | grep -E "gcc|cpp" | head
find "$PREFIX/usr" -name cc1 -exec ls -la {} \;
du -sh "$PREFIX"
