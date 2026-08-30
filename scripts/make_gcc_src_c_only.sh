#!/usr/bin/env bash
# GCC のソース木から「C だけを組むのに要るぶん」を切り出す。
#
# **これまで手作業だった。**その結果 libiberty/testsuite/Makefile.in を
# 落としてしまい、実機の configure-libiberty が
#
#   config.status: error: cannot find input file: `testsuite/Makefile.in'
#
# で止まった (2026-08-30)。**同じことを繰り返さないよう台本にする。**
#
# ---- 何を落とすか --------------------------------------------------------
#
#   - build-* / stage* / *-stage2 …… 過去のビルド木
#   - C 以外の言語 (ada / go / java / fortran / objc / cp) と、その実行時
#   - testsuite の**中身**。**ただし Makefile.in は残す** ——
#     configure が AC_CONFIG_FILES で作るので、無いと config.status が落ちる
#   - .git / doc の生成物など、組むのに要らないもの
#
#   bash scripts/make_gcc_src_c_only.sh [元] [出力先]
#     既定: ports/gcc-4.7.4-aarch64 -> out/gcc-src-c-only
set -euo pipefail

SRC="${1:-ports/gcc-4.7.4-aarch64}"
DST="${2:-out/gcc-src-c-only}"

[ -d "$SRC" ] || { echo "★ 元が無い: $SRC" >&2; exit 1; }
case "$DST" in
    out/*) : ;;
    *) echo "★ 出力先は out/ の下にすること: $DST" >&2; exit 1;;
esac

echo "--- $SRC -> $DST"
rm -rf "$DST"
mkdir -p "$DST"
# **中身をまるごと写してから削る。**「要るものを選ぶ」より「要らないものを
# 落とす」ほうが、見落としたときに"多い"側に倒れる
cp -a "$SRC/." "$DST/"

# 過去のビルド木
find "$DST" -maxdepth 1 -type d \( -name 'build-*' -o -name 'stage*' \) -prune -exec rm -rf {} +
# C 以外の言語とその実行時
for d in ada go java fortran objc objcp cp libada libgo libjava libgfortran \
         libobjc libstdc++-v3 boehm-gc zlib/contrib libjava-* gnattools libitm \
         libquadmath libssp libgomp libmudflap libffi; do
    rm -rf "$DST/$d"
done
rm -rf "$DST/gcc/ada" "$DST/gcc/go" "$DST/gcc/java" "$DST/gcc/fortran" \
       "$DST/gcc/objc" "$DST/gcc/objcp" "$DST/gcc/cp"
rm -rf "$DST/.git"

# **testsuite は中身だけ落とし、Makefile.in は残す。**
# ここが 2026-08-30 に踏んだところ
while IFS= read -r d; do
    keep=""
    [ -f "$d/Makefile.in" ] && keep="$(cat "$d/Makefile.in")"
    rm -rf "$d"
    if [ -n "$keep" ]; then
        mkdir -p "$d"
        printf '%s\n' "$keep" > "$d/Makefile.in"
    fi
done < <(find "$DST" -type d -name testsuite)

# **配線証明。**「作れた」だけでは、要るものが落ちていても気づけない
fail=0
for f in configure depcomp gcc/Makefile.in libiberty/configure \
         libiberty/testsuite/Makefile.in libcpp/configure \
         libdecnumber/configure fixincludes/configure zlib/configure; do
    if [ -e "$DST/$f" ]; then
        printf '  ok   %s\n' "$f"
    else
        printf '  NG   %s が無い\n' "$f" >&2; fail=1
    fi
done
[ "$fail" -eq 0 ] || { echo "★ 要るものが落ちている" >&2; exit 1; }

echo "--- 出来上がり: $(find "$DST" -type f | wc -l) ファイル / $(du -sh "$DST" | cut -f1)"
