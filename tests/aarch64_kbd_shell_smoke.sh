#!/bin/bash
# USB キーボードで ash を操作できることのスモーク (QEMU virt)。
#
# **これが通ると「HDMI とキーボードだけで完結した計算機」になる。**
# シリアルは要らない (このスモークでは判定のために見ているだけ)。
#
# 通る道:
#   USB HID -> xHCI 割り込みEP -> usage を ASCII に変換 (Shift も)
#   -> タイマ割り込みでコンソールのリングへ -> 寝ている ash を起こす
#   -> ash が読む -> 出力が画面とシリアルの両方に出る
#
# **タイマから拾うのが要点。**取りに来られたときだけポーリングする作りだと、
# シェルが寝ているあいだ誰も起こしに来ない。
#
#   make aarch64-kbd-shell-smoke
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
[ -n "$QEMU_BIN" ] || { echo "qemu-system-aarch64 not found" >&2; exit 1; }

KERNEL=out/kernel-aarch64.elf
BUSYBOX=out/busybox-aarch64-musl.elf
DISK=out/aarch64-kbdash.img
FSDIR=out/aarch64-kbdash-fs
LOG=LOGs/aarch64-kbd-shell.log
MON="${TMPDIR:-/tmp}/orthox-ks-$$.sock"

[ -f "$KERNEL" ]  || { echo "missing $KERNEL" >&2; exit 1; }
[ -f "$BUSYBOX" ] || { echo "missing $BUSYBOX ('make aarch64-busybox-musl')" >&2; exit 1; }

rm -rf "$FSDIR"; mkdir -p "$FSDIR/bin" "$FSDIR/etc"
for a in ash echo cat ls uname; do cp "$BUSYBOX" "$FSDIR/bin/$a"; done
printf 'hello from aarch64 xv6fs rootfs\n' > "$FSDIR/etc/motd"
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$FSDIR/aarch64-m4.txt"
rm -f "$DISK"
XV6FS_FSSIZE=16384 XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$FSDIR" "$DISK" > /dev/null

rm -f "$LOG" "$MON"
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$MON"
}
trap cleanup EXIT

"$QEMU_BIN" -machine virt -cpu cortex-a72 -m 512M -smp 1 -display none \
    -serial file:"$LOG" -monitor unix:"$MON",server,nowait \
    -device bochs-display -device qemu-xhci -device usb-kbd \
    -drive file="$DISK",if=none,format=raw,id=vblk0 \
    -device virtio-blk-device,drive=vblk0 \
    -kernel "$KERNEL" &
QEMU_PID=$!

for _ in {1..90}; do
    grep -aq "built-in shell (ash)" "$LOG" 2>/dev/null && break
    sleep 1
done
if ! grep -aq "built-in shell (ash)" "$LOG" 2>/dev/null; then
    echo "*** ash が立ち上がらなかった" >&2
    tail -20 "$LOG" >&2
    exit 1
fi
sleep 2

# **USB キーボードから打つ。** シリアルには何も送らない
python3 - "$MON" <<'PYEOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
time.sleep(0.3); s.recv(65536)
def typ(keys):
    for k in keys:
        s.sendall(("sendkey " + k + "\n").encode())
        time.sleep(0.35)
# **"-" は minus。** shift-minus は "_" になる (実測で uname _m になった) —
# Shift の変換が効いている証拠でもある
typ(["u", "n", "a", "m", "e", "spc", "minus", "m", "ret"])   # uname -m
time.sleep(1.5)
typ(["e", "c", "h", "o", "spc", "shift-k", "shift-b", "shift-d", "ret"])  # echo KBD
time.sleep(2); s.close()
PYEOF

for _ in {1..10}; do
    grep -aq "KBD" "$LOG" 2>/dev/null && break
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- シェルの様子 ---"
tr -d '\r' < "$LOG" | tail -12
echo "--------------------"

LOGN="$LOG.nocr"
tr -d '\r' < "$LOG" > "$LOGN"

echo "--- 判定 ---"

grep -aq "fb pci    : " "$LOGN"                      # 画面が取れている
grep -aq "usb kbd   : ok (boot protocol)" "$LOGN"    # キーボードが立ち上がった
grep -aq "built-in shell (ash)" "$LOGN"

# ---- ★ ここが本番 --------------------------------------------------------
#
# **打った文字が ash に届いて、コマンドとして実行されたこと。**
# エコーだけなら「文字が届いた」までしか言えない。**実行結果**を見る。
#
#   uname -m  -> aarch64          (小文字と '-' が通っている)
#   echo KBD  -> KBD              (**Shift が効いている**)
# **打った行にはプロンプト "# " が付く。**行全体のアンカーは当たらない
grep -aq "# uname -m" "$LOGN"
grep -aq "# echo KBD" "$LOGN"
# **出力行にはプロンプトが付かない。**こちらはアンカーで見る —
# エコーだけ通っていて実行されていない場合を弾くため
grep -aq "^aarch64$" "$LOGN"
grep -aq "^KBD$"     "$LOGN"

if grep -aq "PANIC\|aarch64-exception-BAD" "$LOGN"; then
    echo "*** 例外が出た" >&2
    exit 1
fi

echo "aarch64 USB keyboard shell smoke test: PASS"
