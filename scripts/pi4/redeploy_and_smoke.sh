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
# ★★★ 未実機検証。★★★ この作業環境にPi4本体が繋がっていないため
# (/dev/ttyUSB* が無い、2026-09-16 時点)、このスクリプト自体は実機で
# 1度も走らせていない。初回は手元で見ながら実行し、想定通りに動くか
# 確認すること。想定通り動かなければ、物理的な電源入れ直し+
# tests/aarch64_pi4_serial_ash_smoke.sh の従来手順に戻す。
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

: > "$RAW"
cat "$PORT" >> "$RAW" &
READER_PID=$!
cleanup() {
    [ -n "${READER_PID:-}" ] && kill "$READER_PID" 2>/dev/null || true
    [ -n "${READER_PID:-}" ] && wait "$READER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 否定判定ではないのでset -eをすり抜ける心配は無いが、待ちのループは
# 明示的にreturnで抜ける形にしておく (日報2026-08-09 追9-6と同じ流儀)
wait_for() {   # $1 = 探す文字列, $2 = 制限秒
    local pat="$1" limit="$2" i=0
    while [ "$i" -lt $(( limit * 10 )) ]; do
        if grep -aqF -- "$pat" "$RAW" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        i=$(( i + 1 ))
    done
    return 1
}

printf '/reboot\r' > "$PORT"

if ! wait_for "reboot: 機械をリセットする" 5; then
    echo "*** /reboot の応答が5秒来ない。ashが固まっている可能性がある" >&2
    echo "*** ここはこのスクリプトでは自動化できない。物理的な電源入れ直しが要る" >&2
    tail -c 400 "$RAW" | tr -d '\r' | sed 's/^/***   /' >&2
    exit 1
fi
echo "reboot(2) を送った。新しい起動を待つ (最大 ${REBOOT_TIMEOUT}秒)"

wait_prompt() {   # $1 = 制限秒
    local limit="$1" i=0 seen
    while [ "$i" -lt $(( limit * 10 )) ]; do
        seen="$(tail -c 8 "$RAW" 2>/dev/null | tr -d '\r')"
        case "$seen" in
            *'# ') return 0 ;;
        esac
        sleep 0.1
        i=$(( i + 1 ))
    done
    return 1
}

if ! wait_prompt "$REBOOT_TIMEOUT"; then
    echo "*** ${REBOOT_TIMEOUT}秒待っても新しいプロンプトが出てこない" >&2
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
