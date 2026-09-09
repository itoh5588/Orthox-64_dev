#!/bin/bash
# P4 (実機): Raspberry Pi 4 で動いている ash を、シリアル越しに叩いて検査する。
#
# tests/aarch64_ash_smoke.sh の実機版。**あちらは QEMU を起動するが、
# こちらは既に起動している Pi 4 に繋ぐだけ。**カーネルもディスクも
# このスクリプトは作らない (実機の rootfs 更新は Pi 側で dd する必要があり、
# 日報2026-08-15 §6 の手順を通す)。
#
# 前提:
#   - Pi 4 が Orthox-64 で起動していて、/bin/ash がプロンプトを出している
#   - 変換器が WSL に見えている  (usbipd attach --wsl --busid 1-8)
#
# 使い方:
#   tests/aarch64_pi4_serial_ash_smoke.sh
#   ORTHOX_PI4_PORT=/dev/ttyUSB1 tests/aarch64_pi4_serial_ash_smoke.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

PORT="${ORTHOX_PI4_PORT:-/dev/ttyUSB0}"
BAUD="${ORTHOX_PI4_BAUD:-115200}"
RAW=LOGs/pi4-serial-raw.log        # 受信した生バイト (プロンプト待ちの判定用)
LOG=LOGs/pi4-ash-serial.log        # コマンドの応答だけを繋いだもの (判定用)

# **書き込みは /tmp の下だけ。** 実機の SD に残るので、末尾で片付ける
TMPBASE=/tmp/pi4smoke

# 否定判定は必ずこの関数を通す。`! grep` は set -e の対象外で素通りする
# (日報2026-08-09 追9-6)
must_not() {   # $1 = 固定文字列, $2 = ログ, $3 = 説明 (任意)
    if grep -aqF -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た: $1  ($2)" >&2
        [ -n "${3:-}" ] && echo "*** $3" >&2
        grep -aF -- "$1" "$2" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

# --- 接続 ------------------------------------------------------------------

if [ ! -c "$PORT" ]; then
    echo "*** $PORT が無い" >&2
    echo "*** 変換器が WSL に渡っていない。Windows 側で次を実行する:" >&2
    echo "***   usbipd attach --wsl --busid 1-8" >&2
    exit 1
fi
if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
    echo "*** $PORT を読み書きできない (dialout グループに入っているか確認する)" >&2
    exit 1
fi

# clocal = モデム制御線を見ない。**3 本結線の変換器では DCD が上がらない**ので、
# これが無いと open(2) が返らないことがある
stty -F "$PORT" "$BAUD" cs8 -cstopb -parenb -crtscts clocal raw -echo

READER_PID=""
cleanup() {
    [ -n "$READER_PID" ] && kill "$READER_PID" 2>/dev/null || true
    [ -n "$READER_PID" ] && wait "$READER_PID" 2>/dev/null || true
}
trap cleanup EXIT

: > "$RAW"
: > "$LOG"
cat "$PORT" >> "$RAW" &
READER_PID=$!

# --- 送受信 ----------------------------------------------------------------

# **プロンプトが返るまで待つ。**固定の待ち時間で区切ると、重いコマンドで
# 途中まで読んで次を打ってしまう。
#
# **見るのは受信全体ではなく「今回送ったぶんから後ろ」だけ。**全体の末尾を見ると
# 直前のコマンドが残したプロンプトに当たってしまい、待たずに素通りする
wait_prompt() {   # $1 = 開始オフセット, $2 = 制限秒
    local from="$1" limit="${2:-15}" i=0 seen
    while [ "$i" -lt $(( limit * 10 )) ]; do
        seen="$(tail -c "+$(( from + 1 ))" "$RAW" 2>/dev/null | tr -d '\r' | tail -c 8)"
        case "$seen" in
            *'# ')
                sleep 0.15   # プロンプトの後ろに続くぶんを取りこぼさない
                return 0 ;;
        esac
        sleep 0.1
        i=$(( i + 1 ))
    done
    return 1
}

run_cmd() {   # $1 = コマンド, $2 = 制限秒 (任意)
    local cmd="$1" limit="${2:-15}" before
    before="$(stat -c %s "$RAW")"
    printf '%s\r' "$cmd" > "$PORT"
    if ! wait_prompt "$before" "$limit"; then
        echo "*** ${limit} 秒待ってもプロンプトが返らない: $cmd" >&2
        echo "*** ここまでの受信:" >&2
        tail -c 400 "$RAW" | tr -d '\r' | sed 's/^/***   /' >&2
        exit 1
    fi
    # 応答部分だけを切り出して積む (ash が打った文字をエコーするので
    # コマンド自身も入る。判定には邪魔にならない)
    tail -c "+$(( before + 1 ))" "$RAW" >> "$LOG"
}

# **背景ジョブは run_cmd では待てない。**ash は `&` を見た時点でプロンプトを
# 出してしまい、**子の出力はその後ろに届く**。区間の末尾はプロンプトではなく
# 子の出力になるので、「末尾がプロンプト」の判定に当たらない。
# ここだけ「プロンプトが現れたら、子のぶんを待ってから切り出す」形にする
run_cmd_bg() {   # $1 = コマンド, $2 = 子の出力を待つ秒 (任意)
    local cmd="$1" settle="${2:-3}" before i=0 seen
    before="$(stat -c %s "$RAW")"
    printf '%s\r' "$cmd" > "$PORT"
    while [ "$i" -lt 150 ]; do
        seen="$(tail -c "+$(( before + 1 ))" "$RAW" 2>/dev/null | tr -d '\r')"
        case "$seen" in
            *'# '*) break ;;      # 末尾一致ではなく「含む」。プロンプトの後ろに子の出力が続く
        esac
        sleep 0.1
        i=$(( i + 1 ))
    done
    if [ "$i" -ge 150 ]; then
        echo "*** 15 秒待ってもプロンプトが返らない: $cmd" >&2
        tail -c 400 "$RAW" | tr -d '\r' | sed 's/^/***   /' >&2
        exit 1
    fi
    sleep "$settle"
    tail -c "+$(( before + 1 ))" "$RAW" >> "$LOG"
}

# **最初に素の改行で同期する。**プロンプトが返らないなら、電源が入っていないか、
# 何かのプログラムが前面で走っている
printf '\r' > "$PORT"
if ! wait_prompt 0 5; then
    echo "*** 5 秒待っても ash のプロンプトが返らない ($PORT)" >&2
    echo "*** 確認すること: Pi の電源 / Orthox が起動しているか / 別のプログラムが走っていないか" >&2
    echo "*** ここまでの受信 (空なら 1 バイトも来ていない):" >&2
    tail -c 400 "$RAW" | tr -d '\r' | sed 's/^/***   /' >&2
    exit 1
fi
: > "$LOG"   # 同期ぶんは判定に入れない

echo "--- Pi 4 実機 ash スモーク ($PORT $BAUD) ---"

run_cmd 'echo interactive-ok'
run_cmd 'pwd'
run_cmd 'x=42; echo val=$x'
run_cmd 'uname -m'
# **外部 exec。** fork + execve で SD (xv6fs) の ELF を読む
run_cmd '/bin/echo external-exec-ok'
# パイプ (fork 2 つ + pipe)
run_cmd 'echo a b c | wc'
# 読み込みリダイレクト。**Orthox の rootfs であることの確認も兼ねる**
run_cmd 'cat /etc/motd'
# 書き込み / 読み戻し / 追記
run_cmd "echo redirect-ok > $TMPBASE.txt"
run_cmd "cat $TMPBASE.txt"
run_cmd "echo append-ok >> $TMPBASE.txt"
run_cmd "cat $TMPBASE.txt | wc -l"
# ディレクトリ操作
run_cmd "mkdir ${TMPBASE}d 2>/dev/null; test -d ${TMPBASE}d && echo mkdir-ok"
# unlink
run_cmd "rm $TMPBASE.txt; cat $TMPBASE.txt || echo unlink-ok"
# バックグラウンドジョブ (子が 1 度だけ走ること)
run_cmd_bg 'BG=ok; /bin/echo bg-job-$BG &'
run_cmd 'echo after-bg-ok'
# 算術展開 (CONFIG_FEATURE_SH_MATH)
run_cmd 'i=6; echo math=$((i*7+0))'
# nanosleep のタイマー起床。ここで止まるなら寝たきり
run_cmd 'sleep 1; echo sleep-ok' 20

# **`exit` は送らない。**実機の ash は init なので、抜けるとカーネルが
# bootstrap user exit で終わり、次の電源投入まで戻らない

echo "--- 受信ログ ---"
cat "$LOG"
echo "----------------"

# --- 判定 ------------------------------------------------------------------

echo "--- 実機 ash の判定 ---"

# **判定は CR を除いたコピーに当てる。**
# カーネルは termios の ONLCR に従って LF を CRLF で出す (実機のシリアル端末は
# LF だけでは行頭に戻らないため)。そのままだと行の中身が "aarch64\r" になり、
# `^aarch64$` のような行末アンカーが当たらない (日報2026-08-15 §12)。
# **表示は元のログ、判定はこちら**。
tr -d '\r' < "$LOG" > "$LOG.nocr"
JLOG="$LOG.nocr"

must_not "PANIC"          "$JLOG"
must_not "not found"      "$JLOG" "applet が /bin に無い (fork/exec の故障と紛らわしい)"
must_not "bootstrap user exit" "$JLOG" "ash が終了してしまった (実機は再起動しないと戻らない)"

grep -aq "interactive-ok"   "$JLOG"
grep -aq "^/"               "$JLOG"          # pwd
grep -aq "val=42"           "$JLOG"          # 変数展開
grep -aq "^aarch64$"        "$JLOG"          # uname -m
grep -aq "external-exec-ok" "$JLOG"          # fork + execve
grep -aqE "1 +3 +6"         "$JLOG"          # echo a b c | wc

# **Orthox の rootfs に繋がっていること。**Raspberry Pi OS を起動したまま
# 叩いていると、ここで落ちる
grep -aq "hello from aarch64 xv6fs rootfs" "$JLOG"

grep -aq "redirect-ok"      "$JLOG"
grep -aqE "^ *2$"           "$JLOG"          # 追記が効いて 2 行
grep -aq "mkdir-ok"         "$JLOG"
grep -aq "unlink-ok"        "$JLOG"
grep -aq "bg-job-ok"        "$JLOG"
[ "$(grep -ac "bg-job-ok" "$JLOG")" = "1" ]  # 子が 1 度だけ
grep -aq "after-bg-ok"      "$JLOG"
grep -aq "math=42"          "$JLOG"
grep -aq "sleep-ok"         "$JLOG"

# --- 後始末 ----------------------------------------------------------------
# **実機の SD に残るので消す。**判定の後に置いてあるのは、
# 失敗したときに中身を見に行けるようにするため。
#
# **ディレクトリは `rm -r` で消す。**rootfs の /bin に `rmdir` は無いが、
# busybox の rm は -r を持っている (実機で確認済み)
run_cmd "rm -f $TMPBASE.txt; rm -r ${TMPBASE}d 2>/dev/null; echo cleanup-done"

echo "aarch64 Pi 4 実機 ash smoke test: PASS"
