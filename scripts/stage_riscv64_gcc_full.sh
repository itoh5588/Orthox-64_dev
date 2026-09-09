#!/bin/bash
# Orthox 上で cc1 (GCC の C コンパイラ本体) をビルドするためのツリーを組み立てる。
#
# GCC のソースは 359MB あるが、内訳は
#   testsuite 166MB / ada 47MB / po 38MB / 他ターゲットの config 30MB
# で、C コンパイラのビルドに要るのは ~80MB。剪定して載せる。
#
# configure と gen* (insn-*.c などを生成するプログラム) は Orthox 上で回すと
# 非現実的に遅いので、**生成物はクロス側から持ち込む**。
# つまり「configure からの完全なブートストラップ」ではなく
# 「**344 個の .o を Orthox 上でコンパイルして cc1 をリンクする**」ところまで。
#
# 出力: build/riscv64-gccfull-root/  → rootfs の /src/gcc-full に入る
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GCCSRC="$ROOT/ports/gcc-4.7.4-riscv"
GCCBLD="$GCCSRC/build-orthox"
PREREQ="$ROOT/ports/gcc-prereq-riscv64"
OUT="${1:-$ROOT/build/riscv64-gccfull-root/src/gcc-full}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

[ -d "$GCCBLD/gcc" ] || { echo "error: $GCCBLD が無い。build_gcc474_orthox.sh を先に" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/gcc" "$OUT/build/gcc" "$OUT/prereq"

# --- ソース: 要らないものを外して丸ごと持ってくる ------------------------
echo "--- ソースを剪定して複製"
tar -C "$GCCSRC" -cf - \
    --exclude='testsuite' --exclude='po' --exclude='.git' \
    --exclude='ada' --exclude='fortran' --exclude='go' --exclude='java' \
    --exclude='objc' --exclude='objcp' --exclude='cp' --exclude='lto' \
    --exclude='doc' --exclude='ChangeLog*' --exclude='*.texi' \
    gcc include libcpp libdecnumber libiberty 2>/dev/null | tar -C "$OUT" -xf -

# config は riscv と共通ヘッダだけ残す (他ターゲットで 30MB ある)
if [ -d "$OUT/gcc/config" ]; then
  find "$OUT/gcc/config" -mindepth 1 -maxdepth 1 -type d ! -name riscv ! -name soft-fp \
       -exec rm -rf {} + 2>/dev/null || true
fi

# all-tree.def が言語ディレクトリの tree.def を引くので、その 4 ファイルだけ戻す
for d in ada/gcc-interface cp java objc; do
  mkdir -p "$OUT/gcc/$d"
  cp "$GCCSRC"/gcc/$d/*-tree.def "$OUT/gcc/$d/" 2>/dev/null || true
done

# --- 生成物: クロス側のビルドディレクトリから持ち込む --------------------
# configure の結果 (config.h / tm.h / auto-host.h) と gen* の出力
# (insn-*.c/h, gtype-desc.c/h など)、それに出来上がったライブラリ。
echo "--- 生成物と中間ライブラリを複製"
cp "$GCCBLD"/gcc/*.h "$GCCBLD"/gcc/*.c "$GCCBLD"/gcc/*.def "$OUT/build/gcc/" 2>/dev/null || true
mkdir -p "$OUT/build/libcpp" "$OUT/build/libdecnumber" "$OUT/build/libiberty"
cp "$GCCBLD"/libcpp/*.a       "$OUT/build/libcpp/"       2>/dev/null || true
cp "$GCCBLD"/libdecnumber/*.a "$OUT/build/libdecnumber/" 2>/dev/null || true
cp "$GCCBLD"/libdecnumber/*.h "$OUT/build/libdecnumber/" 2>/dev/null || true
cp "$GCCBLD"/libiberty/*.a    "$OUT/build/libiberty/"    2>/dev/null || true
cp "$GCCBLD"/gcc/*.a          "$OUT/build/gcc/"          2>/dev/null || true

# build/gcc/common/ の生成ヘッダ。無いと riscv-common.c が
#   common/common-target-hooks-def.h: No such file or directory
# で落ちる (実際に踏んだ)。common-targhooks.o は要らないのでヘッダだけ。
mkdir -p "$OUT/build/gcc/common"
cp "$GCCBLD"/gcc/common/*.h   "$OUT/build/gcc/common/"   2>/dev/null || true

# c-family も同じ。生成ヘッダが無いと default-c.c が
#   c-family/c-target-hooks-def.h: No such file or directory
# で落ちる。ここは .o の置き場にもなる (C_OBJS が c-family/*.o を含む)。
mkdir -p "$OUT/build/gcc/c-family"
cp "$GCCBLD"/gcc/c-family/*.h "$OUT/build/gcc/c-family/" 2>/dev/null || true

# zlib。lto-compress.c が zlib.h を要る。libbackend.a に入っていて
# lto-section-in/out.o から参照されるので、無いとリンクが通らない。
# GCC に同梱されており、クロス側でビルド済みのものがある。
mkdir -p "$OUT/build/zlib"
cp "$GCCBLD"/zlib/libz.a      "$OUT/build/zlib/"         2>/dev/null || true
cp "$GCCSRC"/zlib/zlib.h      "$OUT/build/zlib/"         2>/dev/null || true
cp "$GCCBLD"/zlib/zconf.h     "$OUT/build/zlib/"         2>/dev/null || \
  cp "$GCCSRC"/zlib/zconf.h   "$OUT/build/zlib/"         2>/dev/null || true

# gmp/mpfr/mpc (riscv64 用)
cp -a "$PREREQ/include" "$OUT/prereq/"
cp -a "$PREREQ/lib"     "$OUT/prereq/"

# 比較用に、クロス側で作った cc1 を同梱する
mkdir -p "$OUT/ref"
cp "$GCCBLD/gcc/cc1" "$OUT/ref/cc1" 2>/dev/null || true

# --- ビルド対象の一覧を、GCC のリンク構成から起こす ----------------------
#
# 以前はここで `ls *.o` していた。「作るべきもの」を「クロス側で作れたもの」から
# 逆算する形で、依存の向きが逆になっていた:
#
#   - クロス側が作り損ねた .o は一覧に載らず、Orthox 側でも最初から対象外に
#     なる。欠けているのに欠けていることが分からない
#   - objlist はリンク (ar q libbackend.a) も駆動するので、突き合わせても
#     必ず一致する。ものさし自身で自分を測っていて検算にならない
#   - **`ls *.o` は上位階層しか見ない。** C_OBJS に入る c-family/*.o (15 本) と
#     common/common-targhooks.o が構造的に落ちていた
#
# cc1 のリンク構成は GCC の Makefile が持っている。make に展開させて取る
# (build_cc1.sh のファイル別 -D は既にそうしている。対象の一覧だけ ls だった)。
#
#   OBJS                   -> libbackend.a
#   OBJS-libcommon         -> libcommon.a
#   OBJS-libcommon-target  -> libcommon-target.a
#   C_OBJS                 -> cc1 に直接リンク (c-family/*.o を含む)
#   BACKEND の .o          -> main.o
#
# 再帰変数なので `make -p` では展開されない ($(C_AND_OBJC_OBJS) が黙って
# 落ちる)。ダミーの Makefile を重ねて echo させること。
PVMK="$(mktemp)"
printf '__orthox_objs:\n\t@echo $(OBJS) $(OBJS-libcommon) $(OBJS-libcommon-target) $(C_OBJS) $(filter %%.o,$(BACKEND))\n' > "$PVMK"
( cd "$GCCBLD/gcc" && make -f Makefile -f "$PVMK" __orthox_objs 2>/dev/null ) \
    | tr ' ' '\n' | grep '\.o$' | sed 's/\.o$//' | sort -u > "$OUT/build/gcc/objlist.txt"
rm -f "$PVMK"

if [ ! -s "$OUT/build/gcc/objlist.txt" ]; then
    echo "error: objlist を Makefile から起こせなかった ($GCCBLD/gcc/Makefile)" >&2
    exit 1
fi
echo "--- ビルド対象: $(wc -l < "$OUT/build/gcc/objlist.txt") 個 (Makefile のリンク構成から)"

# クロス側に実在する .o と突き合わせる。ここで黙って一覧を縮めない。
# 食い違いは「クロス側のビルドが不完全」を意味するので、報告して止める
( cd "$GCCBLD/gcc" && find . -name '*.o' | sed 's|^\./||; s|\.o$||' ) | sort -u > "$OUT/build/gcc/.crossbuilt.txt"
MISSING="$(comm -23 "$OUT/build/gcc/objlist.txt" "$OUT/build/gcc/.crossbuilt.txt")"
if [ -n "$MISSING" ]; then
    echo "警告: リンク構成にあるのにクロス側で作れていない .o がある:" >&2
    echo "$MISSING" | sed 's/^/    /' >&2
    echo "    (Orthox 側では作れる見込みだが、ref との比較はできない)" >&2
fi
rm -f "$OUT/build/gcc/.crossbuilt.txt"

# 出力先のサブディレクトリを掘っておく。objlist に c-family/xxx のような
# 名前が入るので、build_cc1.sh の -o がディレクトリ不在で落ちないように
( cd "$OUT/build/gcc" && sed -n 's|/[^/]*$||p' objlist.txt | sort -u | while read -r d; do
    [ -n "$d" ] && mkdir -p "$d"
done )

# --- Orthox 上で回すビルドスクリプト -------------------------------------
# make を使わない。make は**レシピ全体を 1 つの長いコマンド文字列として
# /bin/sh に渡す**ので、ループを書くと Orthox の exec の引数上限に当たって
#   make: /bin/sh: No such file or directory
# で落ちる (実際に踏んだ)。素の ash スクリプトなら各コマンドが短く済む。
cat > "$OUT/build/gcc/build_cc1.sh" <<'BUILD_EOF'
#!/bin/sh
# Orthox 上で cc1 の .o を作る。
#   cd /src/gcc-full/build/gcc && sh build_cc1.sh [個数]
#   sh build_cc1.sh seal   ... 既存の .o を完成済みとして記録 (移行用)
#
# 飛ばす判定は done.txt への記録で行う。電源が切れると書きかけの .o が
# 残るが、記録が無いので次回やり直す。.o の有無で判定すると壊れた .o を
# 掴んだまま最後のリンクまで気付けない。印を 1 個ずつ置かないのは
# ディレクトリのエントリ数を増やさないため (列挙の上限に当たる)。
SRCDIR=../../gcc
PREREQ=../../prereq
CC=/usr/bin/gcc

CFLAGS="-c -std=gnu89 -w -O1 -DIN_GCC -DHAVE_CONFIG_H -I. -I$SRCDIR -I$SRCDIR/."
CFLAGS="$CFLAGS -I$SRCDIR/../include -I$SRCDIR/../libcpp/include -I$PREREQ/include"
CFLAGS="$CFLAGS -I$SRCDIR/../libdecnumber -I$SRCDIR/../libdecnumber/dpd -I../libdecnumber"

# ファイル別に要る -D。GCC の Makefile が CFLAGS-<obj> で足しているもので、
# 無いと 'TARGET_NAME undeclared' のように落ちる (実際に 3 本落とした)。
# 値は build-orthox/gcc の Makefile を make に展開させて取った。
P1='-DGCC_INCLUDE_DIR="/usr/lib/gcc/riscv64-linux-musl/4.7.4/include"'
P2='-DFIXED_INCLUDE_DIR="/usr/lib/gcc/riscv64-linux-musl/4.7.4/include-fixed"'
P3='-DLOCAL_INCLUDE_DIR="/usr/local/include" -DCROSS_INCLUDE_DIR="//include"'
P4='-DTOOL_INCLUDE_DIR="/usr/lib/gcc/riscv64-linux-musl/4.7.4/../../../../riscv64-linux-musl/include"'
P5='-DNATIVE_SYSTEM_HEADER_DIR="/include" -DPREFIX="/usr/"'
P6='-DSTANDARD_EXEC_PREFIX="/usr/lib/gcc/" -DTARGET_SYSTEM_ROOT="/"'
P7='-DGPLUSPLUS_INCLUDE_DIR="/usr/include/c++/4.7.4" -DGPLUSPLUS_INCLUDE_DIR_ADD_SYSROOT=0'
P8='-DGPLUSPLUS_TOOL_INCLUDE_DIR="/usr/include/c++/4.7.4/riscv64-linux-musl"'
P9='-DGPLUSPLUS_BACKWARD_INCLUDE_DIR="/usr/include/c++/4.7.4/backward"'
PPD="$P1 $P2 $P3 $P4 $P5 $P6 $P7 $P8 $P9"

DONE=done.txt
[ -f "$DONE" ] || : > "$DONE"

if [ "$1" = seal ]; then
    : > "$DONE"
    s=0
    for n in $(cat objlist.txt); do
        if [ -f "$n.o" ]; then echo "$n" >> "$DONE"; s=$((s+1)); fi
    done
    echo "SEALED $s"
    exit 0
fi

LIMIT=${1:-0}
total=$(wc -l < objlist.txt)
i=0; ok=0; ng=0; skip=0
echo "CC1BUILD-START total=$total"

for n in $(cat objlist.txt); do
    i=$((i+1))
    [ "$LIMIT" -gt 0 ] && [ "$i" -gt "$LIMIT" ] && break
    if grep -qx "$n" "$DONE" 2>/dev/null; then skip=$((skip+1)); continue; fi
    src=""
    for c in "./$n.c" "$SRCDIR/$n.c" "$SRCDIR/config/riscv/$n.c" \
             "$SRCDIR/config/$n.c" \
             "$SRCDIR/c-family/$n.c" "$SRCDIR/common/config/riscv/$n.c"; do
        if [ -f "$c" ]; then src="$c"; break; fi
    done
    if [ -z "$src" ]; then echo "NOSRC $i/$total $n"; ng=$((ng+1)); continue; fi
    xd=""
    case "$n" in
        toplev)     xd='-DTARGET_NAME="riscv64-linux-musl"' ;;
        cppbuiltin) xd="$PPD -DBASEVER=\"4.7.4\"" ;;
        cppdefault) xd="$PPD" ;;
        prefix)     xd='-DPREFIX="/usr" -DBASEVER=4.7.4' ;;
        # 値に空白を入れないこと。$xd は引用符無しで展開するので、
        # 空白があるとそこで単語が割れて gcc に `"` だけが渡る
        # (本家の -DPKGVERSION="(GCC) " をそのまま書いて踏んだ)
        lto-compress) xd='-I../zlib' ;;
        version)    xd='-DBASEVER="4.7.4" -DDATESTAMP="" -DREVISION=""'
                    xd="$xd"' -DDEVPHASE="" -DPKGVERSION="(GCC)"'
                    xd="$xd"' -DBUGURL="<http://gcc.gnu.org/bugs.html>"' ;;
    esac
    if $CC $CFLAGS $xd "$src" -o "$n.o"; then
        echo "$n" >> "$DONE"
        ok=$((ok+1)); echo "CC $i/$total $n"
    else
        ng=$((ng+1)); echo "NG $i/$total $n"
    fi
done
echo "CC1BUILD-DONE ok=$ok ng=$ng skip=$skip"
BUILD_EOF
chmod +x "$OUT/build/gcc/build_cc1.sh"

echo
echo "組み立て完了: $OUT"
du -sh "$OUT"
