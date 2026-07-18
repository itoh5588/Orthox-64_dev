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

"$SRC/configure" \
  --target="$TARGET" \
  --prefix="$OUT" \
  --syslibdir="$OUT/lib" \
  ${MUSL_CONFIGURE_EXTRA:---enable-shared} \
  CC="$CC" AR="$AR" RANLIB="$RANLIB"

make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)"
make install
