#!/bin/bash
# GCC 自身のソースを Orthox 上でコンパイルするためのツリーを組み立てる。
#
# 狙い: **GCC が自分の RISC-V バックエンド (config/riscv/riscv.c) を
# コンパイルできること**、そして出力がクロス版と一致することの確認。
#
# ディレクトリ構造はホスト側のビルドと同じ深さにする。
# GCC は __FILE__ をアサーション等に埋め込むので、-I の相対パスが
# ずれると .o がバイト単位で一致しなくなり、比較の意味が薄れる。
#
#   /src/gcc-self/gcc/...            ← ソース (../../gcc から見える位置)
#   /src/gcc-self/build/gcc/         ← ここを cwd にしてコンパイル
#
# 出力: build/riscv64-gccself-root/  → rootfs の /src/gcc-self に入る
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GCCSRC="$ROOT/ports/gcc-4.7.4-riscv"
GCCBLD="$GCCSRC/build-orthox"
PREREQ="$ROOT/ports/gcc-prereq-riscv64"
OUT="${1:-$ROOT/build/riscv64-gccself-root/src/gcc-self}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

[ -d "$GCCBLD/gcc" ] || { echo "error: $GCCBLD が無い。build_gcc474_orthox.sh を先に" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/gcc/config/riscv" "$OUT/include" "$OUT/libcpp/include" \
         "$OUT/libdecnumber/dpd" "$OUT/build/gcc" "$OUT/build/libdecnumber" \
         "$OUT/prereq/include"

# --- ソース側 (../../gcc から見える位置) --------------------------------
cp "$GCCSRC"/gcc/*.h                    "$OUT/gcc/"
cp "$GCCSRC"/gcc/config/riscv/*.h       "$OUT/gcc/config/riscv/" 2>/dev/null || true
cp "$GCCSRC"/gcc/config/riscv/*.c       "$OUT/gcc/config/riscv/"
cp "$GCCSRC"/gcc/config/riscv/*.def     "$OUT/gcc/config/riscv/" 2>/dev/null || true
# tm.h が引く config ヘッダは連鎖するので (linux.h → linux-android.h …)、
# 個別に列挙せず gcc/config/ 直下をまとめて入れる。数百KB で済む。
cp "$GCCSRC"/gcc/config/*.h "$GCCSRC"/gcc/config/*.def "$OUT/gcc/config/" 2>/dev/null || true
cp "$GCCSRC"/gcc/*.def                  "$OUT/gcc/" 2>/dev/null || true
# all-tree.def が c-family/c-common.def を引く
mkdir -p "$OUT/gcc/c-family"
cp "$GCCSRC"/gcc/c-family/*.h "$GCCSRC"/gcc/c-family/*.def "$OUT/gcc/c-family/" 2>/dev/null || true
# all-tree.def は --enable-languages に関係なく全言語の tree.def を列挙する
# (configure がソースツリーにある言語ディレクトリを全部並べる)。
# 中身は使わないが #include は解決される必要がある。
for d in ada/gcc-interface cp java objc; do
  mkdir -p "$OUT/gcc/$d"
  cp "$GCCSRC"/gcc/$d/*-tree.def "$OUT/gcc/$d/" 2>/dev/null || true
done
cp -a "$GCCSRC"/include/.               "$OUT/include/"
cp -a "$GCCSRC"/libcpp/include/.        "$OUT/libcpp/include/"
cp "$GCCSRC"/libdecnumber/*.h           "$OUT/libdecnumber/" 2>/dev/null || true
cp "$GCCSRC"/libdecnumber/dpd/*.h       "$OUT/libdecnumber/dpd/" 2>/dev/null || true

# --- 生成ヘッダ (ビルドディレクトリ側) ----------------------------------
# config.h / tm.h / insn-*.h / gtype-desc.h などは configure と gen* が作るもので、
# Orthox 上で作り直すのは現実的でないのでクロス側の成果物を持ち込む。
# .h だけでなく生成 .def (all-tree.def など) も要る
cp "$GCCBLD"/gcc/*.h "$GCCBLD"/gcc/*.def "$OUT/build/gcc/" 2>/dev/null || true
cp "$GCCBLD"/libdecnumber/*.h           "$OUT/build/libdecnumber/" 2>/dev/null || true

# --- gmp/mpfr/mpc のヘッダ ----------------------------------------------
cp "$PREREQ"/include/*.h                "$OUT/prereq/include/"

# --- 比較用に、クロス側で作った .o を同梱する ---------------------------
mkdir -p "$OUT/ref"
cp "$GCCBLD/gcc/riscv.o" "$OUT/ref/riscv.o"

cat > "$OUT/build/gcc/Makefile.self" <<'MAKEFILE_EOF'
# GCC 自身のソースを Orthox 上でコンパイルする。
# 使い方: cd /src/gcc-self/build/gcc && make -f Makefile.self
#
# コンパイル指令はクロス側のビルドログからそのまま持ってきたもの。
# 相対パス (-I../../gcc) を同じにしてあるので、コンパイラが同じ出力を出すなら
# .o はバイト単位で一致するはず (__FILE__ 文字列まで揃う)。

CC = /usr/bin/gcc

CFLAGS = -c -std=gnu89 -w -O1 -DIN_GCC -W -Wall -Wno-narrowing \
	 -Wwrite-strings -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes \
	 -Wmissing-format-attribute -pedantic -Wno-long-long -Wno-variadic-macros \
	 -Wno-overlength-strings -Wold-style-definition -Wc++-compat \
	 -DHAVE_CONFIG_H \
	 -I. -I. -I../../gcc -I../../gcc/. \
	 -I../../gcc/../include -I../../gcc/../libcpp/include \
	 -I../../prereq/include \
	 -I../../gcc/../libdecnumber -I../../gcc/../libdecnumber/dpd \
	 -I../libdecnumber

all: riscv.o

riscv.o: ../../gcc/config/riscv/riscv.c
	$(CC) $(CFLAGS) ../../gcc/config/riscv/riscv.c -o riscv.o
	@echo "gcc-self-compile-ok riscv.o"

clean:
	rm -f riscv.o

.PHONY: all clean
MAKEFILE_EOF

echo "組み立て完了: $OUT"
du -sh "$OUT"
