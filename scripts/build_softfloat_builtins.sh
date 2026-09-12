#!/bin/bash
# **clang 用の soft-float 組み込み関数を作る (libgcc の代わり)。**
#
# aarch64 / riscv64 の musl では long double が **IEEE binary128 (TF モード)** で、
# printf などがそこを通る。clang はこれらを libgcc / compiler-rt に外注するが、
# この機械の clang には x86 用の builtins しか入っておらず、クロスの
# libclang_rt.builtins-*.a が無い。そのため busybox のリンクが
#
#   ld.lld: error: undefined symbol: __addtf3 / __divtf3 / __extenddftf2 ...
#
# で落ちていた (2026-09-12 に実測。全 15 個、すべて binary128 まわり)。
#
# **クロス GCC を一式組まずに済ませる。**リポジトリ既定の道は
# ports/port_{aarch64,riscv}_to_gcc47.sh から GCC 4.7.4 を移植する長い手順だが、
# riscv64 側は ports/gcc-4.6.4-riscv というフォークの木を要求し、入手できない。
# **足りないのは libgcc のごく一部だけ**なので、LLVM の compiler-rt から
# 該当ファイルだけ持ってきて clang で組む。
#
#   ports/orthos-riscv64-musl-gcc.sh は gcc を「libgcc.a の在処を引く」ためだけに
#   使うので、ここで作った libgcc.a を指せば、**clang だけで riscv64 の
#   ユーザーランドが組める。**
#
# 使い方: scripts/build_softfloat_builtins.sh <riscv64|aarch64>
set -euo pipefail

ARCH="${1:?usage: build_softfloat_builtins.sh <riscv64|aarch64>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/ports/compiler-rt-builtins"
OUT="$ROOT/ports/builtins-$ARCH"
LLVM_REF="${LLVM_REF:-release/18.x}"
BASE="https://raw.githubusercontent.com/llvm/llvm-project/$LLVM_REF/compiler-rt/lib/builtins"

case "$ARCH" in
    riscv64) TARGET="riscv64-linux-musl"; EXTRA="-march=rv64gc -mabi=lp64d" ;;
    aarch64) TARGET="aarch64-linux-musl"; EXTRA="" ;;
    *) echo "error: 未知のアーキ $ARCH" >&2; exit 1 ;;
esac

HEADERS="int_types.h int_lib.h int_util.h int_math.h int_endianness.h
         fp_lib.h fp_extend.h fp_trunc.h fp_compare_impl.inc fp_add_impl.inc
         fp_mul_impl.inc fp_div_impl.inc fp_extend_impl.inc fp_trunc_impl.inc
         fp_fixint_impl.inc fp_fixuint_impl.inc fp_mode.h"
SOURCES="addtf3.c subtf3.c multf3.c divtf3.c comparetf2.c
         extenddftf2.c extendsftf2.c trunctfdf2.c trunctfsf2.c
         fixtfsi.c fixunstfsi.c floatsitf.c floatunsitf.c fp_mode.c"

mkdir -p "$SRC"
for f in $HEADERS $SOURCES; do
    [ -f "$SRC/$f" ] || curl -sfL -o "$SRC/$f" "$BASE/$f"
done

rm -rf "$OUT"; mkdir -p "$OUT/obj"
for c in $SOURCES; do
    clang --target="$TARGET" $EXTRA -ffreestanding -fno-PIE -O2 \
          -DCOMPILER_RT_HAS_FLOAT16=0 -nostdinc \
          -I"$ROOT/ports/musl-install-$ARCH/include" -I"$SRC" \
          -c "$SRC/$c" -o "$OUT/obj/${c%.c}.o"
done
llvm-ar rcs "$OUT/libgcc.a" "$OUT"/obj/*.o 2>/dev/null || ar rcs "$OUT/libgcc.a" "$OUT"/obj/*.o
echo "built $OUT/libgcc.a ($(llvm-nm "$OUT/libgcc.a" 2>/dev/null | grep -c ' T ' || echo '?') symbols)"
