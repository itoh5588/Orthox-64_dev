#!/usr/bin/env bash
# G-1 の台本 (scripts/pi4/g1_configure.sh) の完了検出を、実機なしで確かめる。
#
# **日報2026-08-29 は完了の誤検出を 3 回踏んでいる。**原因は全部同じで、
# ログ全体を見ていたことと、打った文字のエコーに一致したこと:
#
#   1. `NATIVE-RC=$?` という**打った文字そのもの**に一致した
#   2. ログの前のほうにある**古いエラー行**に一致した
#   3. **古い実行の `CONF-RC=127`** に一致し、「終了した」と誤報した
#
# ここでは、その 3 つをそのまま入力に混ぜて、当たらないことを見る。
# **当たらないことを見るのが目的**なので、成功例だけ通しても意味が無い。
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# 関数だけ読む
# shellcheck disable=SC1091
G1_LIB_ONLY=1 . scripts/pi4/g1_configure.sh

fails=0
total=0
check() {   # $1 = 説明, $2 = 期待, $3 = 実際
    total=$(( total + 1 ))
    if [ "$2" = "$3" ]; then
        printf '  ok   %s\n' "$1"
    else
        printf '  NG   %s  期待=[%s] 実際=[%s]\n' "$1" "$2" "$3" >&2
        fails=$(( fails + 1 ))
    fi
}

TAG=G1_121212_7
OTHER=G1_121212_6

# --- 1. 打った行のエコーには当たらない ------------------------------------
#
# ash は打った行をそのまま返す。区間には必ず `RC=$?` が含まれる
echoed='# cd /gb && sh configure $CF 2>&1; echo "<<'"$TAG"'_RC=$?>>"'
check "打った行のエコーには当たらない" "" "$(printf '%s\n' "$echoed" | marker_rc "$TAG")"

# --- 2. 別の合言葉の印には当たらない (古い実行の残り) ----------------------
old_marker="<<${OTHER}_RC=127>>"
check "別の合言葉には当たらない" "" "$(printf '%s\n' "$old_marker" | marker_rc "$TAG")"

# --- 3. 昔の形式 (合言葉なし) には当たらない -------------------------------
legacy='CONF-RC=127
NATIVE-RC=0'
check "昔の CONF-RC / NATIVE-RC には当たらない" "" "$(printf '%s\n' "$legacy" | marker_rc "$TAG")"

# --- 4. 本物は拾う ---------------------------------------------------------
real="$echoed
checking build system type... aarch64-linux-musl
<<${TAG}_RC=0>>"
check "本物 (RC=0) を拾う" "0" "$(printf '%s\n' "$real" | marker_rc "$TAG")"

real77="$echoed
<<${TAG}_RC=77>>"
check "本物 (RC=77) を拾う" "77" "$(printf '%s\n' "$real77" | marker_rc "$TAG")"

# --- 5. エコーと本物が両方ある区間で、本物だけを拾う ------------------------
mixed="$echoed
$old_marker
$legacy
[cpu] 60s  cpu0 100%(lk25 sd0 rq0)
<<${TAG}_RC=1>>"
check "混ざっていても本物だけ拾う" "1" "$(printf '%s\n' "$mixed" | marker_rc "$TAG")"

# --- 6. 最初の 1 つを拾う (印が 2 つ出ても後ろに引きずられない) -------------
twice="<<${TAG}_RC=0>>
<<${TAG}_RC=5>>"
check "印が 2 つなら最初のもの" "0" "$(printf '%s\n' "$twice" | marker_rc "$TAG")"

# --- 7. 合言葉が前方一致で混ざらない ---------------------------------------
#
# G1_121212_7 と G1_121212_77 は前方一致する。**後者の印を前者と読んではいけない**
check "前方一致する別の合言葉には当たらない" "" \
      "$(printf '%s\n' "<<${TAG}7_RC=0>>" | marker_rc "$TAG")"

if [ "$fails" -gt 0 ]; then
    echo "pi4 g1 marker test: FAIL ($fails / $total 件)" >&2
    exit 1
fi
echo "pi4 g1 marker test: PASS ($total 件)"
