#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="$ROOT/ports/musl-install"
ROOTFS="$ROOT/rootfs"

echo "Populating rootfs for C development environment (musl-only, NO libgcc)..."

# 1. Headers
echo "Copying musl headers..."
mkdir -p "$ROOTFS/usr/include"
cp -r "$SYSROOT/include/." "$ROOTFS/usr/include/"

# 2. Libraries and objects (musl-only)
echo "Copying musl libraries and startup objects..."
mkdir -p "$ROOTFS/usr/lib"
cp "$SYSROOT/lib/libc.a" "$ROOTFS/usr/lib/"
cp "$SYSROOT/lib/crt1.o" "$ROOTFS/usr/lib/"
cp "$SYSROOT/lib/crti.o" "$ROOTFS/usr/lib/"
cp "$SYSROOT/lib/crtn.o" "$ROOTFS/usr/lib/"

# 3. Binaries
echo "Copying native toolchain binaries..."
mkdir -p "$ROOTFS/usr/bin"
cp "$ROOT/ports/gcc-4.7.4/build-musl/gcc/xgcc" "$ROOTFS/usr/bin/gcc"
cp "$ROOT/ports/gcc-4.7.4/build-musl/gcc/cc1" "$ROOTFS/usr/bin/cc1"
cp "$ROOT/ports/binutils-2.26/binutils-2.26/build-musl/gas/as-new" "$ROOTFS/usr/bin/as"
cp "$ROOT/ports/binutils-2.26/binutils-2.26/build-musl/ld/ld-new" "$ROOTFS/usr/bin/ld"

chmod +x "$ROOTFS/usr/bin/"*

echo "Done."
