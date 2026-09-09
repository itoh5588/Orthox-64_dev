#!/bin/bash
# DOOM を USB キーボードで操作できることのスモーク (QEMU virt)。
#
# **画面とキーボードが同時に揃う機械は virt しかない。**
#   raspi4b  画面はあるが PCIe が無いので USB が無い
#   virt     USB は使えるが mailbox が無いので、PCI の表示装置で画面を作る
#
# 通る道 (全段):
#   USB HID -> xHCI 割り込みEP -> usage を scancode に変換 -> 押下/解放の差分
#   -> キュー -> ORTH_SYS_GET_KEY_EVENT -> DOOM の DG_GetKey
#
# **判定は DOOM 側のログから取る。**
#
# 画素の差分では証明にならない。操作前後で 84% の画素が変わるが、
# **デモ自体が動いているので当然**であって、キーが効いた証拠にならない
# (実測でそう見えた)。doomgeneric_orthos.c が出す [doomkey] を見る。
#
#   make aarch64-doom-key-smoke
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
[ -n "$QEMU_BIN" ] || { echo "qemu-system-aarch64 not found" >&2; exit 1; }

KERNEL=out/kernel-aarch64.elf
DOOM=out/doomgeneric-aarch64.elf
WAD=rootfs/doom1.wad
DISK=out/aarch64-doom-disk.img
FSDIR=out/aarch64-doom-fs
LOG=LOGs/aarch64-doom-key.log
# **短いパスに置く。** AF_UNIX は 108 文字までで、リポジトリの下だと超える
MON="${TMPDIR:-/tmp}/orthox-dk-$$.sock"

[ -f "$KERNEL" ] || { echo "missing $KERNEL" >&2; exit 1; }
[ -f "$DOOM" ]   || { echo "missing $DOOM ('make aarch64-doom')" >&2; exit 1; }
[ -f "$WAD" ]    || { echo "missing $WAD" >&2; exit 1; }

rm -rf "$FSDIR"; mkdir -p "$FSDIR/bin"
cp "$DOOM" "$FSDIR/bin/doom"
cp "$WAD"  "$FSDIR/doom1.wad"
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
    grep -aq "Auto-scaling factor" "$LOG" 2>/dev/null && break
    sleep 1
done
if ! grep -aq "Auto-scaling factor" "$LOG" 2>/dev/null; then
    echo "*** DOOM が起動しなかった" >&2
    tail -20 "$LOG" >&2
    exit 1
fi
sleep 4

# **DOOM が実際に使う組み合わせを送る。**
# Esc でメニュー、Enter で決定、上で前進、Ctrl で撃つ
python3 - "$MON" <<'PYEOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
time.sleep(0.3); s.recv(65536)
for k in ["esc", "ret", "up", "ctrl"]:
    s.sendall(("sendkey " + k + "\n").encode())
    time.sleep(1.0)
time.sleep(2); s.close()
PYEOF

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- DOOM に届いたキー ---"
tr -d '\r' < "$LOG" | grep -a "doomkey" | head -20
echo "-------------------------"

LOGN="$LOG.nocr"
tr -d '\r' < "$LOG" > "$LOGN"

echo "--- 判定 ---"

# 画面が取れていること。**掴めていないと DOOM は真っ黒で走り続ける**
grep -aq "fb pci    : " "$LOGN"
grep -aq "Framebuffer mapped at" "$LOGN"
# キーボードが立ち上がっていること
grep -aq "usb kbd   : ok (boot protocol)" "$LOGN"

# ---- ★ ここが本番 --------------------------------------------------------
#
# scancode と DOOM のキーコードの両方を見る。**scancode だけだと
# 変換表が壊れていても通る**
grep -aq "\[doomkey\] pressed=1 scan=0x01 ascii=0x1b doom=0x1b" "$LOGN"   # Esc
grep -aq "\[doomkey\] pressed=1 scan=0x1c ascii=0x0d doom=0x0d" "$LOGN"   # Enter
grep -aq "\[doomkey\] pressed=1 scan=0xc8 ascii=0x00 doom=0xad" "$LOGN"   # 上 = KEY_UPARROW
grep -aq "\[doomkey\] pressed=1 scan=0x1d ascii=0x00 doom=0xa3" "$LOGN"   # Ctrl = KEY_FIRE

# **離したことも届くこと。** DOOM は「押した」で歩き始め「離した」で止まる。
# ここが出ないと歩きっぱなしになる
grep -aq "\[doomkey\] pressed=0 scan=0xc8 ascii=0x00 doom=0xad" "$LOGN"

count=$(grep -ac "doomkey" "$LOGN")
echo "  DOOM に届いたキーイベント: $count"
# 押下 4 + 解放 4 = 8 が期待値
if [ "$count" -lt 6 ]; then
    echo "*** 届いたイベントが少なすぎる ($count)" >&2
    exit 1
fi

if grep -aq "PANIC\|aarch64-exception-BAD" "$LOGN"; then
    echo "*** 例外が出た" >&2
    exit 1
fi

echo "aarch64 DOOM key smoke test: PASS"
