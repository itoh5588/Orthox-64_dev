#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL_ROOT="$ROOT/ports/musl-install"
INCLUDEDIR="$MUSL_ROOT/include"
LIBDIR="$MUSL_ROOT/lib"
GCC_INCLUDEDIR="$(gcc -print-file-name=include)"
LIBGCC="$(gcc -print-libgcc-file-name)"
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
    gcc
    -static
    -fno-PIC
    -fno-PIE
    -no-pie
    -D__ORTHOS__
    -nostdinc
    -I"$INCLUDEDIR"
    -I"$GCC_INCLUDEDIR"
    -L"$LIBDIR"
    -L"$LIBGCC_DIR"
)

if ! $compile_only && ! $reloc_link; then
    cmd+=(
        -nostdlib
        "$MUSL_ROOT/lib/crt1.o"
        "$MUSL_ROOT/lib/crti.o"
        "$(gcc -print-file-name=crtbeginT.o)"
        "${args[@]}"
        -Wl,--start-group
        -lc
        -lm
        "$LIBGCC"
        -Wl,--end-group
        "$(gcc -print-file-name=crtend.o)"
        "$MUSL_ROOT/lib/crtn.o"
    )
else
    cmd+=("${args[@]}")
fi

exec "${cmd[@]}"
