#!/bin/bash
# Rpi4実機へ現在のHEADのカーネルを配って、物理的な電源入れ直しなしで
# smokeまで回す。
#
# **reboot(2)を実機のashから叩いて再起動させる**
# (日報2026-08-29 §9、R-1。`/reboot`は静的リンクの道具で、
# scripts/build_rootfs_aarch64_selfhost.shがrootfsへ必ず入れる。実測21〜27秒)。
# reboot(2)はSoCを本当にリセットするので、その後に現れるashのプロンプトは
# 必ず新しく起動したカーネルのもの (旧カーネルのプロセスはリセットで消える)。
#
# dev_up.sh (立ち上げ) と tests/aarch64_pi4_serial_ash_smoke.sh (判定) の
# 間を橋渡しするだけで、判定ロジック自体は持たない。
#
# 前提:
#   - Pi4がOrthox-64で起動していて、/bin/ashのプロンプトが (今は) 出ている
#     (= scripts/pi4/dev_up.sh は済んでいる)
#   - **シェルが応答すること。**前面のプログラムが固まっていると
#     `/reboot`が打てず、このスクリプトは失敗する。**そのときは物理的な
#     電源入れ直しが要る (このスクリプトでは代替できない)**
#
# 使い方:
#   bash scripts/pi4/dev_up.sh          # まだなら先に
#   bash scripts/pi4/redeploy_and_smoke.sh
#   ORTHOX_PI4_PORT=/dev/ttyUSB1 bash scripts/pi4/redeploy_and_smoke.sh
#
# 実機で PASS 済み (日報2026-09-16、2026-09-18)。
#
# 受信は dev_up.sh の常時キャプチャがあればそのログから読み、自分では
# tty を開かない (無ければ自分で cat を立てて LOGs/pi4-redeploy-raw.log に取る)。
# 同じ tty を 2 つで開くとバイトがランダムに分かれて両方壊れる
# (scripts/pi4/g1_configure.sh と同じ理由)。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

PORT="${ORTHOX_PI4_PORT:-/dev/ttyUSB0}"
BAUD="${ORTHOX_PI4_BAUD:-115200}"
REBOOT_TIMEOUT="${ORTHOX_PI4_REBOOT_TIMEOUT:-60}"
RAW=LOGs/pi4-redeploy-raw.log

if [ ! -c "$PORT" ]; then
    echo "*** $PORT が無い" >&2
    echo "*** scripts/pi4/dev_up.sh は済んでいるか (usbipd attach)" >&2
    exit 1
fi

echo "=== 1. 現在のHEADでkernel8.imgを組んでnetboot配信ルートへ置く ==="
make aarch64-pi4-netboot

echo "=== 2. 実機のashから /reboot を叩く ==="
stty -F "$PORT" "$BAUD" cs8 -cstopb -parenb -crtscts clocal raw -echo

find_capture() {
    local p
    for p in $(pgrep -x cat 2>/dev/null); do
        if tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | grep -qx -- "$PORT"; then
            readlink -f "/proc/$p/fd/1" 2>/dev/null && return 0
        fi
    done
    return 1
}

READER_PID=""
cleanup() {
    [ -n "${READER_PID:-}" ] && kill "$READER_PID" 2>/dev/null || true
    [ -n "${READER_PID:-}" ] && wait "$READER_PID" 2>/dev/null || true
}
trap cleanup EXIT

if CAPTURE="$(find_capture)" && [ -n "$CAPTURE" ]; then
    RAW="$CAPTURE"
    echo "(dev_up.sh のキャプチャに相乗りする: $RAW)"
else
    : > "$RAW"
    cat "$PORT" >> "$RAW" &
    READER_PID=$!
fi

# **見るのは $1 のオフセットから後ろだけ。**キャプチャのログには前回の
# `reboot:` やプロンプトが残っていて、全体を見ると待たずに素通りする
region() {   # $1 = 開始オフセット
    tail -c "+$(( $1 + 1 ))" "$RAW" 2>/dev/null
}

# 否定判定ではないのでset -eをすり抜ける心配は無いが、待ちのループは
# 明示的にreturnで抜ける形にしておく (日報2026-08-09 追9-6と同じ流儀)
wait_for() {   # $1 = 探す文字列, $2 = 制限秒, $3 = 開始オフセット
    local pat="$1" limit="$2" from="$3" i=0
    while [ "$i" -lt $(( limit * 10 )) ]; do
        # -q にしない。pipefail の下では grep が先に抜けると tail が
        # SIGPIPE で落ち、一致したのに失敗扱いになることがある
        if region "$from" | grep -aF -- "$pat" >/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    return 1
}

BEFORE_REBOOT="$(stat -c %s "$RAW")"
printf '/reboot\r' > "$PORT"

if ! wait_for "reboot: 機械をリセットする" 5 "$BEFORE_REBOOT"; then
    echo "*** /reboot の応答が5秒来ない。ashが固まっている可能性がある" >&2
    echo "*** ここはこのスクリプトでは自動化できない。物理的な電源入れ直しが要る" >&2
    tail -c 400 "$RAW" | tr -d '\r' | sed 's/^/***   /' >&2
    exit 1
fi
echo "reboot(2) を送った。新しい起動を待つ (最大 ${REBOOT_TIMEOUT}秒)"
# リセットの印が出た後ろだけを見る。旧カーネルの出力に当てない
AFTER_REBOOT="$(stat -c %s "$RAW")"

# **プロンプトではなく ash の起動バナーを待つ。**起動直後のプロンプト `# ` には
# カーネルの [ep0]/[lwip] の出力が同じ行で続けて届き、「末尾が `# `」の瞬間は
# ほとんど無い (2026-09-18 に 4 起動すべてで確認、そのせいで 60 秒待って落ちた)。
# プロンプトとの同期は smoke 側が改行を送ってやり直す
if ! wait_for "built-in shell (ash)" "$REBOOT_TIMEOUT" "$AFTER_REBOOT"; then
    echo "*** ${REBOOT_TIMEOUT}秒待っても新しいashが起動してこない" >&2
    echo "*** netboot配信 (TFTP) かconfig.txtを疑う (scripts/pi4/README.md)" >&2
    echo "*** 最悪、物理的な電源入れ直しで復旧を試す" >&2
    tail -c 400 "$RAW" | tr -d '\r' | sed 's/^/***   /' >&2
    exit 1
fi
echo "新しい起動を確認した"

cleanup
trap - EXIT

echo "=== 3. ash smokeで判定する ==="
exec bash "$SCRIPT_DIR/../../tests/aarch64_pi4_serial_ash_smoke.sh"
