#!/bin/bash
set -e

# Mac ARM 上で x86_64 ELF バイナリをビルドするためのカナディアン・クロスコンパイル設定
# (ビルド: aarch64-apple-darwin, ホスト: x86_64-orthos, ターゲット: x86_64-orthos)

export PATH="/opt/homebrew/opt/x86_64-elf-gcc/bin:$PATH"

# orthOS 向けの sysroot (ヘッダ・ライブラリ) パス
SYSROOT="$(pwd)/../user"

# binutils-2.26 の configure がテストプログラムをコンパイルする際、
# crt0.o などのスタートアップコードが見つからずにコケる問題を回避するため
# CFLAGS, LDFLAGS に細かく設定を入れる。
export CC="x86_64-elf-gcc"
export CXX="x86_64-elf-g++"
export AR="x86_64-elf-ar"
export RANLIB="x86_64-elf-ranlib"

# Configure 用のフラグ
export CFLAGS="-O2 -I$SYSROOT/include"
# リンクが通るように LDFLAGS を設定。
export LDFLAGS="-static -nostartfiles -L$SYSROOT/libs $SYSROOT/crt0.o $SYSROOT/syscalls.o $SYSROOT/user_stubs.o -lc"

cd binutils-2.26/binutils-2.26
mkdir -p build && cd build

# ホスト(実行環境)を x86_64-elf とし、スタティックリンクでビルド
# 実行に必要な関数を autoconf に認識させるため、環境変数でキャッシュをセットする。
# getcwd, fcntl, lstat などの代替実装がコンパイルでエラーになるのを防ぐため、
# libiberty の configure でそれらがあるとして扱う。
export ac_cv_func_getcwd=yes
export ac_cv_header_fcntl_h=yes
export ac_cv_header_sys_stat_h=yes
export ac_cv_func_lstat=yes
export ac_cv_func_stat=yes
export ac_cv_func_open=yes

../configure \
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

make -j$(sysctl -n hw.ncpu) CFLAGS="-O2 -I$SYSROOT/include -D_getopt_internal=getopt_internal -Dlstat=stat" LDFLAGS="-static -nostartfiles -L$SYSROOT/libs $SYSROOT/crt0.o $SYSROOT/syscalls.o -lc"
