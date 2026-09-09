#!/bin/bash
# orthos-riscv64-musl-gcc.sh の GCC 版。
# 移植した GCC 4.7.4 (stage-2) + ports/musl-install-riscv64 で Orthox 向けの
# riscv64 musl 静的バイナリを作る。busybox のビルドから ORTHOS_CC として呼ばれる。
#
# clang 版との違い:
#   - clang 固有のフラグ (--target= / -fuse-ld=lld) を使わない
#   - libgcc は同じ GCC のものを -print-libgcc-file-name で引く
#     (clang 版は homebrew の riscv64-elf-gcc から借りていた)
# それ以外 (crt1/crti/crtn を自前で並べる、-Ttext 0x400000、
#  -lc -lm libgcc を --start-group で囲む) は clang 版と同じ。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL_ROOT="${ORTHOS_SYSROOT:-$ROOT/ports/musl-install-riscv64}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$MUSL_ROOT/include}"
LIBDIR="$MUSL_ROOT/lib"

GCC_BIN="${ORTHOS_RISCV64_GCC474:-$ROOT/ports/cross-riscv64/bin/riscv64-linux-musl-gcc}"
if [ ! -x "$GCC_BIN" ]; then
    echo "error: $GCC_BIN が無い。ports/build_gcc474_riscv_stage2.sh を先に通すこと" >&2
    exit 1
fi

ARCH=(-march=rv64gc -mabi=lp64d)
LIBGCC="$("$GCC_BIN" "${ARCH[@]}" -print-libgcc-file-name)"
LIBGCC_DIR="$(dirname "$LIBGCC")"

raw_args=("$@")
args=()
compile_only=false
reloc_link=false

for arg in "${raw_args[@]}"; do
    case "$arg" in
        -c|-E|-S)
            compile_only=true
            ;;
        -r|-Wl,-r)
            reloc_link=true
            ;;
        -lc|-lz|-lgcc_s|-lpthread|-ldl|-lrt|*/libz.so*|*/libgcc_s.so*)
            continue
            ;;
        */crt0.o|*/crti.o|*/crtn.o|*/crtbegin*.o|*/crtend*.o|*/rcrt1.o)
            continue
            ;;
        -fuse-ld=*)
            continue
            ;;
        -fno-PIE)
            args+=("-fno-pie")
            continue
            ;;
    esac
    args+=("$arg")
done

cmd=(
    "$GCC_BIN"
    "${ARCH[@]}"
    -static
    -fno-PIC
    -fno-pie
    -D__ORTHOS__
    -nostdinc
    -I"$INCLUDEDIR"
    -L"$LIBDIR"
    -L"$LIBGCC_DIR"
)

if ! $compile_only && ! $reloc_link; then
    cmd+=(
        -nostdlib
        -Wl,-Ttext,0x400000
        "$MUSL_ROOT/lib/crt1.o"
        "$MUSL_ROOT/lib/crti.o"
        "${args[@]}"
        -Wl,--start-group
        -lc
        -lm
        "$LIBGCC"
        -Wl,--end-group
        "$MUSL_ROOT/lib/crtn.o"
    )
elif $reloc_link; then
    cmd+=(-nostdlib "${args[@]}")
else
    cmd+=("${args[@]}")
fi

exec "${cmd[@]}"
