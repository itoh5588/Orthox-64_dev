#!/usr/bin/env bash
# 手元のファイルを、シリアル越しに実機へ置く。
#
# **rootfs を焼き直さずに 1 ファイルだけ直したいとき**に使う。
# GCC のソース木のように「イメージに焼いてから足りないと分かった」ものを、
# 再配布なしで足せる。
#
# ---- なぜ 1 行ずつ送るのか ------------------------------------------------
#
# **一度に流すと文字が落ちる。**2026-08-30 に 10 行ほどのヒアドキュメントを
# まとめて送ったら途中で切れ、シェルが継続待ち (`>`) のまま固まった。
# コンソールの受信リングが溢れる。1 行ずつ、間を置いて送る。
#
# ---- なぜ引用符つきヒアドキュメントなのか --------------------------------
#
# `cat > f <<'EOF'` の中では**何も解釈されない**。$ も ` も \ も " も '
# もそのまま通る (echo や printf で組み立てると $ が展開される)。
#
# ---- ★ tab だけは通らない ------------------------------------------------
#
# **実機の行編集が tab を食う** (タブ補完として消費される)。
# 2026-08-30、2,673 バイトの Makefile.in を送ったら 2,654 バイトで着いた。
# **足りない 19 バイトは、中に在る tab の数 19 とちょうど一致した。**
#
# なので tab を目印に置き換えて送り、実機側で awk が戻す。
# **sed ではなく awk。**置換側の `\t` をタブとして確実に解釈する。
#
#   bash scripts/pi4/push_file.sh <手元のファイル> <実機の置き場>
#   bash scripts/pi4/push_file.sh a.txt /src/a.txt
#
#   PI4_TTY       既定 /dev/ttyUSB0
#   PUSH_DELAY    1 行あたりの間 (秒)。既定 0.30。落ちるなら増やす
set -uo pipefail

SRC="${1:-}"
DST="${2:-}"
PORT="${PI4_TTY:-/dev/ttyUSB0}"
DELAY="${PUSH_DELAY:-0.30}"
EOFTAG="ORTHOX_PUSH_END_$$"

if [ -z "$SRC" ] || [ -z "$DST" ]; then
    echo "使い方: bash scripts/pi4/push_file.sh <手元のファイル> <実機の置き場>" >&2
    exit 1
fi
[ -f "$SRC" ] || { echo "*** $SRC が無い" >&2; exit 1; }
[ -c "$PORT" ] || { echo "*** $PORT が無い。dev_up.sh を先に" >&2; exit 1; }

TABTAG="@@ORTHOX_TAB@@"

# **区切りと目印が中身に現れないこと。**現れると途中で閉じて残りが
# コマンドになるか、戻すときに余計なタブが生える
if grep -qF "$EOFTAG" "$SRC"; then
    echo "*** 区切り $EOFTAG が中身に在る" >&2; exit 1
fi
if grep -qF "$TABTAG" "$SRC"; then
    echo "*** tab の目印 $TABTAG が中身に在る" >&2; exit 1
fi

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG=""
for p in $(pgrep -x cat 2>/dev/null); do
    if tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | grep -qx -- "$PORT"; then
        LOG="$(readlink -f "/proc/$p/fd/1" 2>/dev/null)"; break
    fi
done
[ -n "$LOG" ] || { echo "*** $PORT のキャプチャが無い。dev_up.sh を先に" >&2; exit 1; }

lines=$(wc -l < "$SRC")
bytes=$(wc -c < "$SRC")
echo "--- $SRC ($bytes バイト / $lines 行) を $DST へ ---"

send() { printf '%s\r' "$1" > "$PORT"; sleep "$DELAY"; }

# 置き場のディレクトリを先に作る
send "busybox mkdir -p \"$(dirname "$DST")\""
send "cat > \"$DST.push\" <<'$EOFTAG'"
n=0
while IFS= read -r line || [ -n "$line" ]; do
    # tab を目印に置き換える (行編集に食われるため)
    send "${line//$'\t'/$TABTAG}"
    n=$(( n + 1 ))
    if [ $(( n % 20 )) -eq 0 ]; then printf '  %d/%d 行\n' "$n" "$lines"; fi
done < "$SRC"
send "$EOFTAG"
sleep 1
# **目印を tab に戻す。**awk は置換側の \t をタブとして解釈する
send "busybox awk '{gsub(/$TABTAG/,\"\\t\"); print}' \"$DST.push\" > \"$DST\""
send "rm -f \"$DST.push\""
sleep 1

# **中身まで照合する。**「置けた」だけでは、途中で落ちていても気づけない
want_md5="$(md5sum "$SRC" | cut -d' ' -f1)"
mark="PUSH_$$"
before=$(stat -c %s "$LOG")
printf 'busybox md5sum "%s"; busybox wc -c "%s"; echo "<<%s_RC=$?>>"\r' "$DST" "$DST" "$mark" > "$PORT"
i=0
while [ "$i" -lt 60 ]; do
    tail -c "+$(( before + 1 ))" "$LOG" | tr -d '\r' | grep -aqE "<<${mark}_RC=[0-9]+>>" && break
    sleep 2; i=$(( i + 2 ))
done
got="$(tail -c "+$(( before + 1 ))" "$LOG" | tr -d '\r' | grep -aoE '^[0-9a-f]{32}' | tail -1)"
echo "  手元: $want_md5"
echo "  実機: ${got:-(取れなかった)}"
if [ "$got" = "$want_md5" ]; then
    echo "  ok   md5 が一致した"
    exit 0
fi
echo "*** md5 が違う。PUSH_DELAY を増やして送り直すこと" >&2
tail -c "+$(( before + 1 ))" "$LOG" | tr -d '\r' | grep -av '^\[' | tail -4 | sed 's/^/***   /' >&2
exit 1
