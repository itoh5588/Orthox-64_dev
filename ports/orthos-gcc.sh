#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/user}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
GCC_INCLUDEDIR="$(gcc -print-file-name=include)"
LIBGCC="$(gcc -print-libgcc-file-name)"
LIBGCC_DIR="$(dirname "$LIBGCC")"
LIBDIR="$ROOT/user/libs"
CRT0_O="${ORTHOS_CRT0:-$ROOT/user/crt0.o}"
TLS_O="${ORTHOS_TLS_O-$ROOT/user/tls.o}"
ARCH_PRCTL_O="${ORTHOS_ARCH_PRCTL_O-$ROOT/user/arch_prctl.o}"
RUST_STUBS_O="${ORTHOS_RUST_STUBS_O-$ROOT/ports/rust_stubs.o}"
NEWLIB_STUBS_O="${ORTHOS_NEWLIB_STUBS_O-$ROOT/ports/newlib_stubs.o}"
SYSCALLS_O="${ORTHOS_SYSCALLS_O:-$ROOT/user/syscalls.o}"
SYSCALL_WRAP_O="${ORTHOS_SYSCALL_WRAP_O:-$ROOT/build/newlib/user/syscall_wrap.o}"
runtime_objs=()
CRTBEGIN_T_O="$(gcc -print-file-name=crtbeginT.o)"
CRTEND_O="$(gcc -print-file-name=crtend.o)"
musl_startup=false

if [ -f "$SYSROOT/lib/crt1.o" ] && [ -f "$SYSROOT/lib/crti.o" ] && [ -f "$SYSROOT/lib/crtn.o" ]; then
    musl_startup=true
fi

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
        -lc|-lgcc_s|-lpthread|-ldl|-lrt|*/libgcc_s.so*)
            continue
            ;;
        */crt0.o|*/crti.o|*/crtn.o|*/crtbegin*.o|*/crtend*.o|*/rcrt1.o)
            continue
            ;;
    esac
    args+=("$arg")
done

for obj in "$TLS_O" "$ARCH_PRCTL_O" "$RUST_STUBS_O" "$NEWLIB_STUBS_O" "$SYSCALLS_O" "$SYSCALL_WRAP_O"; do
    if [ -n "$obj" ] && [ -f "$obj" ]; then
        runtime_objs+=("$obj")
    fi
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
    -L"$SYSROOT/lib"
    -L"$LIBDIR"
    -L"$LIBGCC_DIR"
    "${args[@]}"
)

if ! $compile_only && ! $reloc_link; then
    if $musl_startup; then
        cmd+=(
            -nostdlib
            "$SYSROOT/lib/crt1.o"
            "$SYSROOT/lib/crti.o"
            "$CRTBEGIN_T_O"
            -Wl,--start-group
            "${runtime_objs[@]}"
            -lc
            -lm
            "$LIBGCC"
            -Wl,--end-group
            "$CRTEND_O"
            "$SYSROOT/lib/crtn.o"
        )
    else
        cmd+=(
            -nostdlib
            -Wl,--wrap=signal
            -Wl,-u,main
            "$CRT0_O"
            -Wl,--start-group
            "${runtime_objs[@]}"
            "$LIBGCC"
            -lstdc++
            -lm
            -lc
            -Wl,--end-group
        )
    fi
fi

exec "${cmd[@]}"
