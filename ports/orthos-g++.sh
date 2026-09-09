#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/user}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
LIBDIR="$ROOT/user/libs"
CRT0_O="${ORTHOS_CRT0:-$ROOT/user/crt0.o}"
SYSCALLS_O="${ORTHOS_SYSCALLS_O:-$ROOT/user/syscalls.o}"
SYSCALL_WRAP_O="${ORTHOS_SYSCALL_WRAP_O:-$ROOT/build/newlib/user/syscall_wrap.o}"
NEWLIB_STUBS_O="${ORTHOS_NEWLIB_STUBS_O-$ROOT/ports/newlib_stubs.o}"
runtime_objs=()
CRTBEGIN_T_O="$(g++ -print-file-name=crtbeginT.o)"
CRTEND_O="$(g++ -print-file-name=crtend.o)"
musl_startup=false

if [ -f "$SYSROOT/lib/crt1.o" ] && [ -f "$SYSROOT/lib/crti.o" ] && [ -f "$SYSROOT/lib/crtn.o" ]; then
    musl_startup=true
fi

raw_args=("$@")
args=()
link=true

for arg in "${raw_args[@]}"; do
    case "$arg" in
        -c|-E|-S)
            link=false
            ;;
    esac
    args+=("$arg")
done

for obj in "$SYSCALLS_O" "$SYSCALL_WRAP_O" "$NEWLIB_STUBS_O"; do
    if [ -n "$obj" ] && [ -f "$obj" ]; then
        runtime_objs+=("$obj")
    fi
done

cmd=(
    g++
    -static
    -fno-PIC
    -fno-PIE
    -no-pie
    -fno-exceptions
    -fno-rtti
    -DHAVE_STDLIB_H
    -DHAVE_STRING_H
    -DHAVE_UNISTD_H
    -DHAVE_SYS_STAT_H
    -DHAVE_SYS_TYPES_H
    -DHAVE_LIMITS_H
    -L"$SYSROOT/lib"
    -L"$LIBDIR"
    "${args[@]}"
    -idirafter "$INCLUDEDIR"
)

if $link; then
    if $musl_startup; then
        cmd+=(
            -nostdlib
            "$SYSROOT/lib/crt1.o"
            "$SYSROOT/lib/crti.o"
            "$CRTBEGIN_T_O"
            "${runtime_objs[@]}"
            -lstdc++
            -lm
            -lc
            "$(g++ -print-libgcc-file-name)"
            "$CRTEND_O"
            "$SYSROOT/lib/crtn.o"
        )
    else
        cmd+=(
            "$CRT0_O"
            "${runtime_objs[@]}"
            -lstdc++
            -lm
            -lc
        )
    fi
fi

exec "${cmd[@]}"
