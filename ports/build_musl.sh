#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/ports/musl}"
OUT="${2:-$ROOT/ports/musl-install}"
TARGET="x86_64-linux-musl"

if [ ! -d "$SRC" ]; then
  echo "musl source not found: $SRC" >&2
  echo "place musl sources under ports/musl or pass the source path explicitly" >&2
  exit 1
fi

mkdir -p "$OUT"
cd "$SRC"

CC="clang -target $TARGET -ffreestanding -fno-PIE -fuse-ld=lld"
AR="$(command -v x86_64-elf-ar || echo ar)"
RANLIB="$(command -v x86_64-elf-ranlib || echo ranlib)"

./configure \
  --target="$TARGET" \
  --prefix="$OUT" \
  --syslibdir="$OUT/lib" \
  --enable-shared \
  CC="$CC" AR="$AR" RANLIB="$RANLIB"

make -j"$(nproc 2>/dev/null || echo 1)"
make install
