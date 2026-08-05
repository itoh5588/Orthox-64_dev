#!/bin/bash
# 4 並列でビルドした .o を各イメージから回収し、1 つのツリーに集める。
#
#   scripts/collect_riscv64_gcc_objs.sh [イメージ...]
#
# 並列ビルドは 1 枚のイメージを共有できないので、担当分を書いた objlist を
# 入れた別々のイメージで走らせている。その成果を集める。
#
# **stage_riscv64_gcc_full.sh を再実行しないこと。** 先頭に rm -rf があり、
# 集めた .o ごとツリーを消す。既存の build/riscv64-gccfull-root にそのまま置く。
#
# 集めたあとは make riscv64-rootfs でイメージを作り直す (新規ファイルの追加は
# --replace ではできないため、イメージ再生成が唯一の道)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/build/riscv64-gccfull-root/src/gcc-full/build/gcc"
GUEST=/src/gcc-full/build/gcc
EXTRACT="$ROOT/scripts/extract_rootfs_xv6fs.py"

if [ "$#" -eq 0 ]; then
    echo "使い方: $0 <イメージ1> [イメージ2 ...]" >&2
    exit 1
fi
[ -d "$DEST" ] || { echo "見つからない: $DEST" >&2; exit 1; }

echo "集約先: $DEST"
echo "既にある .o: $(find "$DEST" -maxdepth 1 -name '*.o' | wc -l) 本"
echo

total=0
for img in "$@"; do
    [ -f "$img" ] || { echo "見つからない: $img" >&2; exit 1; }
    names=$(python3 "$EXTRACT" "$img" --list "$GUEST" 2>/dev/null \
            | awk '$NF ~ /\.o$/ {print $NF}')
    n=0
    for f in $names; do
        # 既にあるものは飛ばす (どのイメージにも先行 99 本が入っているため)
        if [ -f "$DEST/$f" ]; then continue; fi
        python3 "$EXTRACT" "$img" "$GUEST/$f" "$DEST/$f" >/dev/null 2>&1 || {
            echo "  取り出し失敗: $f ($img)" >&2; exit 1; }
        n=$((n + 1))
    done
    total=$((total + n))
    echo "$(basename "$img"): $(echo "$names" | wc -w) 本中 $n 本を新規に取得"
done

echo
echo "新規取得: $total 本"
echo "集約後の .o: $(find "$DEST" -maxdepth 1 -name '*.o' | wc -l) 本"

# 完成印を作り直す。これが無いと新しいイメージで全部ビルドし直しになる。
# 「.o が実際にある」ことだけを根拠にする (done.txt を継ぎ足さない)。
DONE="$DEST/done.txt"
OBJLIST="$DEST/objlist.txt"
: > "$DONE"
cnt=0
missing=""
while read -r n; do
    [ -z "$n" ] && continue
    cnt=$((cnt + 1))
    if [ -f "$DEST/$n.o" ]; then
        echo "$n" >> "$DONE"
    else
        missing="$missing $n"
    fi
done < "$OBJLIST"
echo "done.txt を再生成: $(wc -l < "$DONE") 行 / objlist $cnt 本"

if [ -n "$missing" ]; then
    echo
    echo "まだ無い .o ($(echo "$missing" | wc -w) 本):"
    echo "$missing" | tr ' ' '\n' | grep -v '^$' | sed 's/^/    /'
    echo
    # cc1 のリンクに要らないもの (gcc ドライバや gcc-ar などのソース)。
    # これらが残っていても cc1 は作れる。
    echo "このうち cc1 に不要なもの: gcc gcc-ar gcc-nm gcc-ranlib gccspec cppspec"
    echo "                          collect2 collect2-aix gcov gcov-dump lto-wrapper tlink"
    echo "必要なものが残っている場合は、新しいイメージで build_cc1.sh を"
    echo "もう一度回せば拾える (-D 付きの版が入っている)。"
fi
