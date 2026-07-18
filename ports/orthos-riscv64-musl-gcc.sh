#!/bin/bash
# riscv64-linux-musl 静的リンク用コンパイラドライバ (orthos-musl-gcc.sh の riscv64 版)
# homebrew LLVM clang + ports/musl-install-riscv64 sysroot + riscv64-elf-gcc の libgcc を使う
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL_ROOT="$ROOT/ports/musl-install-riscv64"
INCLUDEDIR="$MUSL_ROOT/include"
LIBDIR="$MUSL_ROOT/lib"

CLANG_BIN="${ORTHOS_RISCV64_CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
RVGCC_BIN="${ORTHOS_RISCV64_GCC:-/opt/homebrew/bin/riscv64-elf-gcc}"
LIBGCC="$("$RVGCC_BIN" -march=rv64gc -mabi=lp64d -print-libgcc-file-name)"
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
    esac
    args+=("$arg")
done

cmd=(
    "$CLANG_BIN"
    --target=riscv64-linux-musl
    -march=rv64gc
    -mabi=lp64d
    -static
    -fno-PIC
    -fno-PIE
    -D__ORTHOS__
    -nostdinc
    -I"$INCLUDEDIR"
    -L"$LIBDIR"
    -L"$LIBGCC_DIR"
)

if ! $compile_only && ! $reloc_link; then
    cmd+=(
        -fuse-ld=lld
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
    cmd+=(-fuse-ld=lld -nostdlib "${args[@]}")
else
    cmd+=("${args[@]}")
fi

exec "${cmd[@]}"
