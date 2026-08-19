#!/bin/bash
# USB HID キーボードのスモーク (QEMU virt)。
#
# **実機の Pi 4 では踏めない。** Pi 4 の USB は PCIe の先の VL805 で、
# QEMU の raspi4b は PCIe を持っていない (brcm,bcm2711-pcie が無効)。
# **QEMU virt が唯一の検証の場**なので、ここを厚くしておく。
#
# 通る道:
#   DTB の pci-host-ecam-generic -> ECAM 走査 -> BAR 割り当て -> xHCI 初期化
#   -> HID インターフェース検出 -> 割り込みエンドポイント -> レポート
#   -> usage を scancode/ascii に変換 -> 押した/離したの差分
#
# キーは QEMU の monitor の sendkey で送る。**画面は要らない。**
#
#   make aarch64-usb-kbd-smoke
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-aarch64 not found" >&2
    exit 1
fi

KERNEL=out/kernel-aarch64.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL" >&2; exit 1; }

LOG=LOGs/aarch64-usb-kbd.log
# **monitor のソケットは短いパスに置く。** AF_UNIX は 108 文字までで、
# リポジトリの下に置くと超えることがある (実測で落ちた)
MON="${TMPDIR:-/tmp}/orthox-kbd-$$.sock"

rm -f "$LOG" "$MON"
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$MON"
}
trap cleanup EXIT

# **ブリッジの先に置く。** Raspberry Pi 4 の実機では USB (VL805) が
# ルートポートの先、バス 01 にいる (2026-08-16 実機の lspci で確認)。
# **バス 0 に直付けした構成だけ試していると、実機で届かないことに
# 気づけない。**ORTHOX_USB_KBD_FLAT=1 で直付けにも切り替えられる
if [ -n "${ORTHOX_USB_KBD_FLAT:-}" ]; then
    USB_ARGS=(-device qemu-xhci -device usb-kbd)
    echo "(バス 0 に直付け)"
elif [ -n "${ORTHOX_USB_KBD_HUB:-}" ]; then
    # **実機の Raspberry Pi 4 と同じ配線。**
    # 4 つの Type-A は VL805 内蔵の USB2 ハブの先にあり、xHCI から見えるのは
    # ハブ 1 台だけ (2026-08-17 実機で vid=0x2109 class=0x09 を確認)。
    # **ハブを挟まない構成だけ試していると、B-3 がまるごと未検証になる**
    USB_ARGS=(-device pcie-root-port,id=rp0,chassis=1
              -device qemu-xhci,id=xhci,bus=rp0
              -device usb-hub,bus=xhci.0,port=1,id=hub1
              -device usb-kbd,bus=xhci.0,port=1.1)
    echo "(ルートポート -> xHCI -> USB2 ハブ -> キーボード = 実機と同じ形)"
else
    USB_ARGS=(-device pcie-root-port,id=rp0,chassis=1
              -device qemu-xhci,bus=rp0 -device usb-kbd)
    echo "(ルートポートの先 = 実機と同じ形)"
fi

"$QEMU_BIN" -machine virt -cpu cortex-a72 -m 512M -smp 1 -display none \
    -serial file:"$LOG" -monitor unix:"$MON",server,nowait \
    "${USB_ARGS[@]}" \
    -kernel "$KERNEL" &
QEMU_PID=$!

for _ in {1..60}; do
    grep -aq "usb-kbd-probe-start" "$LOG" 2>/dev/null && break
    sleep 1
done
if ! grep -aq "usb-kbd-probe-start" "$LOG" 2>/dev/null; then
    echo "*** 探針まで来なかった。AARCH64_USB_KBD_PROBE=1 で組んだか確認する" >&2
    tail -20 "$LOG" >&2
    exit 1
fi

# **送るキーは戻り値まで分かっているものを選ぶ。**
# a / x / 上矢印 / 空白 / ctrl — DOOM が実際に使う組み合わせ
python3 - "$MON" <<'PYEOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
time.sleep(0.3); s.recv(65536)
for k in ["a", "x", "up", "spc", "ctrl"]:
    s.sendall(("sendkey " + k + "\n").encode())
    time.sleep(0.8)
time.sleep(1); s.close()
PYEOF

for _ in {1..20}; do
    grep -aq "usb-kbd-probe-done" "$LOG" 2>/dev/null && break
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- USB キーボード Serial Output ---"
# [ctx] は出力デバイス文脈のダンプ。落ちたときの唯一の手がかりなので落とさない
grep -aE "\[usb\]|usb |pci |\[kbd\]|\[ctx\]|usb-kbd-probe" "$LOG" | head -60
echo "-----------------------------------"

LOGN="$LOG.nocr"
tr -d '\r' < "$LOG" > "$LOGN"

must_not() {
    if grep -aqF -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た: $1" >&2
        [ -n "${3:-}" ] && echo "*** $3" >&2
        exit 1
    fi
}

echo "--- 判定 ---"

# 1) PCI から xHCI を見つけて BAR を配れたこと。
#    **BAR が 0 のままだと番地 0 を読みに行って沈黙する**
# **バス番号は固定しない。** 直付けならバス 0、ブリッジの先ならバス 1
grep -aqE "pci 0x[0-9a-f]+:0x[0-9a-f]+\.0x[0-9a-f]+  vid 0x[0-9a-f]+ did 0x[0-9a-f]+ class 0x0c0330 bar0 0x[0-9a-f]+" "$LOGN"
must_not "class 0x0c0330 bar0 0x00000000" "$LOGN" "xHCI の BAR を配れていない"

# 2) HID のインターフェースと割り込みエンドポイントが取れたこと
grep -aq "HID keyboard if=" "$LOGN"
grep -aq "usb kbd   : ok (boot protocol)" "$LOGN"

# 3) **ここが本番: 送ったキーが変換されて出ること**
grep -aq "usb-kbd-probe-done" "$LOGN"

#   sendkey a     -> scancode 0x1e = 30、ascii 'a' = 97
#   sendkey x     -> scancode 0x2d = 45、ascii 'x' = 120
#   sendkey up    -> scancode 0xc8 = 200、ascii 0
#   sendkey spc   -> scancode 0x39 = 57、ascii ' ' = 32
#   sendkey ctrl  -> scancode 0x1d = 29 (修飾キー。usage には現れない)
grep -aq "\[kbd\] down sc=30 ascii=97"  "$LOGN"
grep -aq "\[kbd\] down sc=45 ascii=120" "$LOGN"
grep -aq "\[kbd\] down sc=200 ascii=0"  "$LOGN"
grep -aq "\[kbd\] down sc=57 ascii=32"  "$LOGN"
grep -aq "\[kbd\] down sc=29 ascii=0"   "$LOGN"

# 4) **離したことも出ること。** DOOM は「押した」で歩き始め「離した」で
#    止まるので、解放が出ないと歩きっぱなしになる
grep -aq "\[kbd\] up   sc=30 ascii=97" "$LOGN"

# 5) 受け取った数。**押下 5 + 解放 5 で 10 前後**。少なすぎたら取りこぼし
count=$(grep -a "usb-kbd-probe-done count=" "$LOGN" | head -1 | sed 's/.*count=//')
echo "  受け取ったイベント数: $count"
if [ "$count" -lt 8 ]; then
    echo "*** イベントが少なすぎる ($count)。取りこぼしている" >&2
    exit 1
fi

must_not "PANIC" "$LOGN"
must_not "aarch64-mmu-BAD" "$LOGN"

echo "aarch64 USB keyboard smoke test: PASS"
