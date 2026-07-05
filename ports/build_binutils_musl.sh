#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/ports/binutils-2.26/binutils-2.26"
BUILD="$SRC/build-musl"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/ports/musl-install}"
XAR="$(command -v x86_64-elf-ar || command -v ar)"
XRANLIB="$(command -v x86_64-elf-ranlib || command -v ranlib)"
XCXX="$(command -v x86_64-elf-g++ || command -v g++)"
TOOLWRAP="$BUILD/.toolwrap"

mkdir -p "$TOOLWRAP"
for tool in ar ranlib nm ld as objcopy objdump readelf strip; do
  wrapper="$TOOLWRAP/x86_64-elf-$tool"
  if [ ! -x "$wrapper" ]; then
    host_tool="$(command -v x86_64-elf-$tool || command -v "$tool" || true)"
    if [ -n "$host_tool" ]; then
      ln -sf "$host_tool" "$wrapper"
    fi
  fi
done
ln -sf "$ROOT/ports/orthos-gcc.sh" "$TOOLWRAP/x86_64-elf-gcc"
ln -sf "$ROOT/ports/orthos-gcc.sh" "$TOOLWRAP/x86_64-elf-cc"
ln -sf "$ROOT/ports/orthos-g++.sh" "$TOOLWRAP/x86_64-elf-g++"
ln -sf "$ROOT/ports/orthos-g++.sh" "$TOOLWRAP/x86_64-elf-c++"

export PATH="$TOOLWRAP:/opt/homebrew/opt/x86_64-elf-gcc/bin:$PATH"
export CC="$ROOT/ports/orthos-gcc.sh"
export CXX="$XCXX"
export AR="$XAR"
export RANLIB="$XRANLIB"

export ORTHOS_SYSROOT="$SYSROOT"
export ORTHOS_INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
export ORTHOS_LIBDIR="${ORTHOS_LIBDIR:-$SYSROOT/lib}"
export ORTHOS_CRT0="${ORTHOS_CRT0:-$ROOT/build/musl/user/crt0.o}"
export ORTHOS_TLS_O="${ORTHOS_TLS_O:-}"
export ORTHOS_ARCH_PRCTL_O="${ORTHOS_ARCH_PRCTL_O:-}"
export ORTHOS_RUST_STUBS_O="${ORTHOS_RUST_STUBS_O:-}"
export ORTHOS_NEWLIB_STUBS_O="${ORTHOS_NEWLIB_STUBS_O:-}"
export ORTHOS_SYSCALLS_O="${ORTHOS_SYSCALLS_O:-$ROOT/build/musl/user/syscalls.o}"
export ORTHOS_SYSCALL_WRAP_O="${ORTHOS_SYSCALL_WRAP_O:-$ROOT/build/musl/user/syscall_wrap.o}"

mkdir -p "$BUILD"

if [ -f "$BUILD/Makefile" ] && ! grep -Fq "VPATH=$SRC" "$BUILD/Makefile"; then
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
fi

cd "$BUILD"

export ac_cv_func_getcwd=yes
export ac_cv_header_fcntl_h=yes
export ac_cv_header_sys_stat_h=yes
export ac_cv_func_lstat=yes
export ac_cv_func_stat=yes
export ac_cv_func_open=yes

if [ ! -f Makefile ]; then
  "$SRC"/configure \
    --host=x86_64-elf \
    --target=x86_64-elf \
    --disable-nls \
    --disable-werror \
    --disable-gdb \
    --disable-libdecnumber \
    --disable-readline \
    --disable-sim \
    --disable-shared \
    --enable-static \
    --with-sysroot="$SYSROOT"
fi

NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

make -j"$NCPU" \
  CFLAGS="-O2 -I$SYSROOT/include -D_GNU_SOURCE -Dlstat=stat" \
  all-gas all-ld
