#!/bin/bash
# DOOM を QEMU の raspi4b で動かして、**絵が出ていることを画素で確かめる。**
#
# **「起動した」では足りない。** DOOM は画面が真っ黒でも最後まで走り、
# シリアルには I_InitGraphics まで正常に出る。実際、色順を間違えていたとき
# (pixel order = 1) もログは全部正常だった。**判定は画面から取る。**
#
# 画面は QEMU の monitor の screendump で PPM に落とし、python で数える。
#
#   make aarch64-doom-run   がカーネルの組み直しとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs out

# raspi4b を持つ qemu を探す (tests/aarch64_pi4_smoke.sh と同じ理屈)
QEMU_BIN=""
for cand in "$HOME/qemu-build/build/qemu-system-aarch64" \
            "$HOME/qemu-local/bin/qemu-system-aarch64" \
            "$(command -v qemu-system-aarch64 2>/dev/null || true)"; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    if "$cand" -machine help 2>/dev/null | grep -q "^raspi4b "; then
        QEMU_BIN="$cand"; break
    fi
done
if [ -z "$QEMU_BIN" ]; then
    echo "aarch64 doom run: SKIP (raspi4b を持つ qemu が無い。QEMU 9.0 以降が要る)"
    exit 0
fi

DOOM=out/doomgeneric-aarch64.elf
WAD=rootfs/doom1.wad
IMG=out/kernel8.img
DISK=out/aarch64-doom-disk.img
FSDIR=out/aarch64-doom-fs
LOG=LOGs/aarch64-doom.log
PPM=out/aarch64-doom-screen.ppm
MON=out/aarch64-doom-mon.sock

[ -f "$DOOM" ] || { echo "missing $DOOM ('make aarch64-doom')" >&2; exit 1; }
[ -f "$WAD" ]  || { echo "missing $WAD (doom1.wad が要る)" >&2; exit 1; }
[ -f "$IMG" ]  || { echo "missing $IMG" >&2; exit 1; }

# **専用のディスクを作る。** out/rootfs-*.img には触らない
rm -rf "$FSDIR"; mkdir -p "$FSDIR/bin"
cp "$DOOM" "$FSDIR/bin/doom"
cp "$WAD"  "$FSDIR/doom1.wad"
# カーネルの fs 自己診断が中身まで照合する既知ファイル
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$FSDIR/aarch64-m4.txt"
rm -f "$DISK"
XV6FS_FSSIZE=16384 XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$FSDIR" "$DISK" > /dev/null

rm -f "$LOG" "$PPM" "$MON"
QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$MON"
}
trap cleanup EXIT

"$QEMU_BIN" -machine raspi4b -display none \
    -serial file:"$LOG" -monitor unix:"$MON",server,nowait \
    -kernel "$IMG" -dtb tests/dtb/bcm2711-rpi-4-b.dtb \
    -drive file="$DISK",if=sd,format=raw 2>/dev/null &
QEMU_PID=$!

# 起動を待つ。**PIO の SD 越しに 4MB の WAD を読む**ので時間がかかる
for _ in {1..120}; do
    grep -aq "Auto-scaling factor" "$LOG" 2>/dev/null && break
    sleep 1
done
# デモが数フレーム進むまで待つ (最初の 1 枚は真っ黒なことがある)
sleep 12

python3 - "$MON" "$PPM" <<'PYEOF'
import socket, sys, time
mon, ppm = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX); s.connect(mon)
time.sleep(0.5); s.recv(65536)
s.sendall(b"screendump " + ppm.encode() + b"\n")
time.sleep(2); s.close()
PYEOF

kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- DOOM (raspi4b) Serial Output (末尾) ---"
tail -20 "$LOG"
echo "-------------------------------------------"

echo "--- 判定 ---"
LOGN="$LOG.nocr"
tr -d '\r' < "$LOG" > "$LOGN"

grep -aq "I_InitGraphics: DOOM screen size: w x h: 320 x 200" "$LOGN"
grep -aq "Auto-scaling factor: 2" "$LOGN"
# **フレームバッファを掴めたこと。** 失敗しても DOOM は走り続けるので、
# ここを見ないと「絵が出ない」原因が分からない
grep -aq "Framebuffer mapped at" "$LOGN"
if grep -aq "Failed to map framebuffer\|Failed to get video info" "$LOGN"; then
    echo "*** フレームバッファを掴めていない" >&2
    exit 1
fi

[ -f "$PPM" ] || { echo "*** 画面を取れなかった ($PPM)" >&2; exit 1; }

# ---- ★ ここが本番: 画素を数える ------------------------------------------
python3 - "$PPM" <<'PYEOF'
import sys, collections
d = open(sys.argv[1], "rb").read()
p = d.split(b"\n", 3)
w, h = map(int, p[1].split()); px = p[3]

n = w * h
black = 0
hist = collections.Counter()
rsum = gsum = bsum = 0
for i in range(0, n * 3, 3 * 37):        # 37 画素おきに間引いて数える
    r, g, b = px[i], px[i+1], px[i+2]
    if r < 8 and g < 8 and b < 8: black += 1
    hist[(r >> 5, g >> 5, b >> 5)] += 1
    rsum += r; gsum += g; bsum += b
sampled = len(range(0, n * 3, 3 * 37))

print(f"  画面 {w}x{h}  標本 {sampled}")
print(f"  黒の割合   : {black * 100 // sampled}%")
print(f"  色の種類   : {len(hist)}")
print(f"  平均 R/G/B : {rsum//sampled} / {gsum//sampled} / {bsum//sampled}")

# **真っ黒ではないこと。** 掴み損ねると全面黒のまま走る
if black * 100 // sampled > 80:
    print("*** 画面がほぼ真っ黒。描けていない", file=sys.stderr); sys.exit(1)
# **単色でないこと。** 塗り潰しだけなら色は数種類しか出ない
if len(hist) < 20:
    print(f"*** 色が {len(hist)} 種類しかない。絵になっていない", file=sys.stderr); sys.exit(1)
# **赤と青が入れ替わっていないこと。**
#
# DOOM の画面は茶色 (R > G > B) が支配的。色順を間違えると青に寄る。
# 実測: pixel order = 1 のとき平均が B 優位になり、0 で R 優位に戻った
if rsum <= bsum:
    print(f"*** 赤より青が強い (R={rsum//sampled} B={bsum//sampled})。"
          "色順が逆 (fb.c の pixel order)", file=sys.stderr)
    sys.exit(1)
PYEOF

echo "aarch64 DOOM run: PASS"
