#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/ports/musl}"
OUT="${2:-$ROOT/ports/musl-install}"
TARGET="${3:-x86_64-linux-musl}"
# 第4引数でビルドディレクトリを指定すると out-of-tree ビルド (ソースツリーを汚さない)
BUILD="${4:-$SRC}"

if [ ! -d "$SRC" ]; then
  echo "musl source not found: $SRC" >&2
  echo "place musl sources under ports/musl or pass the source path explicitly" >&2
  exit 1
fi

mkdir -p "$OUT" "$BUILD"
cd "$BUILD"

MUSL_CC_BIN="${MUSL_CC:-clang}"
CC="$MUSL_CC_BIN -target $TARGET ${MUSL_EXTRA_CFLAGS:-} -ffreestanding -fno-PIE -fuse-ld=lld"
AR="${MUSL_AR:-$(command -v x86_64-elf-ar || echo ar)}"
RANLIB="${MUSL_RANLIB:-$(command -v x86_64-elf-ranlib || echo ranlib)}"

# **共有ライブラリを作るときはコンパイラランタイムが要る。**
# aarch64 の long double は binary128 で、比較や四則が __lttf2 / __addtf3 と
# いった呼び出しになる。静的 (libc.a) ではリンクしないので潜っているが、
# libc.so を作る段になって未解決で落ちる。
# clang は -print-libgcc-file-name に compiler-rt の名前を答えるが、
# この環境には aarch64 版が入っていないので、手持ちのクロス GCC の
# libgcc.a を MUSL_LIBCC で明示する (Makefile 側で渡している)。
MUSL_LIBCC_ARG=""
if [ -n "${MUSL_LIBCC:-}" ]; then
  MUSL_LIBCC_ARG="LIBCC=$MUSL_LIBCC"
fi

"$SRC/configure" \
  --target="$TARGET" \
  --prefix="$OUT" \
  --syslibdir="$OUT/lib" \
  ${MUSL_CONFIGURE_EXTRA:---enable-shared} \
  CC="$CC" AR="$AR" RANLIB="$RANLIB" $MUSL_LIBCC_ARG

make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)"
make install
