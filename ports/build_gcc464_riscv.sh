#!/bin/bash
# GCC 4.6.4 (ekaitz-zarraga フォーク / RISC-V バックエンド付き) を
# riscv64-linux-musl 向けクロスコンパイラとして組む。
#
# 目的は「4.6.4 に移植された SiFive 製 RISC-V バックエンドが実際に動くか」の検証。
# 通れば cc1 が rv64gc / lp64d のアセンブリを吐くところまで確認できる。
#
# 前提: x86_64 Linux ホスト。Apple Silicon では組めない (GCC の aarch64 対応は 4.8 から)。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
SRC="$PORTS/gcc-4.6.4-riscv"
BUILD="$SRC/build-riscv64"
PREFIX="$PORTS/cross-riscv64"
PREREQ="$PORTS/gcc-prereq-host"   # gmp/mpfr/mpc (ホスト用)

GMP_VER=6.1.0
MPFR_VER=3.1.4
MPC_VER=1.0.3

JOBS="$(nproc)"

if [ ! -d "$SRC" ]; then
  echo "error: $SRC が無い。先に clone すること:"
  echo "  git clone --depth 1 https://codeberg.org/ekaitz-zarraga/gcc.git $SRC"
  exit 1
fi

# --- 前提ライブラリを別 prefix に組む -------------------------------------
#
# in-tree (gcc/gmp, gcc/mpfr, gcc/mpc に展開する方式) は 4.6.4 では使えない。
# 4.6.4 のトップレベルは MPFR 2.4 時代のフラット配置を前提に
# --with-mpfr-include=<src>/mpfr / --with-mpfr-lib=<build>/mpfr/.libs を渡すが、
# MPFR 3.1.x は中身を src/ へ移したので mpc の configure が mpfr.h を見失う。
# (4.7.4 は新配置に対応しているので fetch_gcc47.sh の in-tree 方式で通る)
build_prereq() {
  local name="$1" ver="$2" tarball="$3" shift_args
  shift 3
  if [ -f "$PREREQ/lib/lib${name}.a" ]; then
    echo "--- ${name}-${ver}: 既にある。skip"
    return
  fi
  echo "--- ${name}-${ver} を組む"
  local work="$PORTS/.prereq-build"
  mkdir -p "$work"
  cd "$work"
  rm -rf "${name}-${ver}"
  tar -xf "$PORTS/$tarball"
  mkdir -p "build-${name}"
  cd "build-${name}"
  rm -rf ./*
  "../${name}-${ver}/configure" --prefix="$PREREQ" --disable-shared --enable-static "$@"
  make -j"$JOBS"
  make install
  cd "$PORTS"
}

build_prereq gmp  "$GMP_VER"  "gmp-${GMP_VER}.tar.bz2"
build_prereq mpfr "$MPFR_VER" "mpfr-${MPFR_VER}.tar.bz2" --with-gmp="$PREREQ"
build_prereq mpc  "$MPC_VER"  "mpc-${MPC_VER}.tar.gz"    --with-gmp="$PREREQ" --with-mpfr="$PREREQ"

# in-tree の残骸があると 4.6.4 のトップレベルがそちらを優先してしまう
rm -rf "$SRC/gmp" "$SRC/mpfr" "$SRC/mpc"

# --- 現代の gcc で 4.6.4 を組むための逃げ ---------------------------------
# 4.6.4 は C89 時代のコードなので、gcc 13 の既定 (-std=gnu17 + 厳しい診断) では通らない。
# 警告はすべて落とし、-O1 に留める。
export CFLAGS="-std=gnu89 -w -O1"
export CXXFLAGS="-std=gnu++98 -w -O1"
export LC_ALL=C

# binutils を先に組んで PATH に入れておくこと。
# アセンブラが見えないと configure が gcc_cv_as='' のまま進み、HAVE_AS_TLS が
# undef になって __thread が emutls (__emutls_get_address) に落ちる。
# musl は native TLS を使うのでそれでは使い物にならない。
if [ ! -x "$PREFIX/bin/riscv64-linux-musl-as" ]; then
  echo "error: riscv64 の as が無い。先に ./build_binutils_riscv64.sh を通すこと"
  exit 1
fi
export PATH="$PREFIX/bin:$PATH"

# --- multilib: Orthox は 2 つの ABI を併用する ---------------------------
#   カーネル / freestanding user … -mabi=lp64  (soft-float ABI)
#   musl ユーザーランド          … -mabi=lp64d
# どちらか一方の libgcc では足りないので multilib で両方作る。
#
# ただし config.gcc が要求する riscv/t-linux-multilib は**フォークに入っていない**
# (上流では生成済みファイルが同梱されている)。同梱の multilib-generator で
# 必要な 2 本ぶんだけ作る。
MULTILIB_TM="$SRC/gcc/config/riscv/t-linux-multilib"
if [ ! -f "$MULTILIB_TM" ]; then
  echo "--- t-linux-multilib を生成 (rv64gc: lp64d / lp64)"
  python3 "$SRC/gcc/config/riscv/multilib-generator" \
    rv64gc-lp64d-- rv64gc-lp64-- > "$MULTILIB_TM"
fi

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

# MAKEINFO=missing: texinfo 7.1 は 4.6.4 同梱の .texi を処理できないので info を作らせない
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
  --without-headers \
  --with-newlib \
  MAKEINFO=missing

echo "=== configure 完了。all-gcc を開始 ==="
make -j"$JOBS" all-gcc

# libgcc まで作る。__divtf3 (musl の printf が踏む 128bit soft-float) の実体はここ。
# --disable-threads が要る: target が riscv64-linux-musl なのでスレッドモデルが
# posix になり、--without-headers なのに pthread.h を要求して落ちる。
echo "=== all-target-libgcc を開始 ==="
make -j"$JOBS" all-target-libgcc

echo
echo "=== 完了 ==="
echo "cc1:    $BUILD/gcc/cc1"
echo "driver: $BUILD/gcc/xgcc"
find "$BUILD" -name libgcc.a | sed 's/^/libgcc: /'
