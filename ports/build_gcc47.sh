#!/bin/bash
set -e

export PATH="/opt/homebrew/opt/x86_64-elf-gcc/bin:$PATH"

# orthOS 向けの sysroot (ヘッダ・ライブラリ) パス
SYSROOT="$(pwd)/../user"

export CC="$(pwd)/orthos-gcc.sh"
export CXX="$(pwd)/orthos-g++.sh"
export AR="x86_64-elf-ar"
export RANLIB="x86_64-elf-ranlib"
export AWK="gawk"
export LC_ALL=C

# Configure 用のフラグ
export CFLAGS="-O2 -std=gnu89"
export CXXFLAGS="-O2 -std=gnu++98"
export LDFLAGS="-static -nostartfiles -L$SYSROOT/libs -L$(pwd) -u _start -Wl,--whole-archive -losstubs -Wl,--no-whole-archive -lc"

cd gcc-4.7.4
mkdir -p build && cd build

# 実行に必要な関数を autoconf に認識させる


# Configure を実行
../configure \
  --target="x86_64-elf" \
  --host="x86_64-elf" \
  --prefix="/usr/local" \
  --disable-nls \
  --enable-languages=c \
  --with-newlib \
  --with-sysroot="$SYSROOT" \
  --without-headers \
  --disable-shared \
  --disable-libssp \
  --disable-libgomp \
  --disable-libmudflap \
  --disable-libitm \
  --disable-libatomic \
  --disable-libquadmath \
  --disable-dependency-tracking \
  am_cv_CC_dependencies_compiler_type=gcc3 \
  am_cv_CXX_dependencies_compiler_type=gcc3 \
  gcc_cv_cxx_dependency_style=gcc3

echo "Configuration finished. Starting build..."
make -j$(sysctl -n hw.ncpu) all-gcc
