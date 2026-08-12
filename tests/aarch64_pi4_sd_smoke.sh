#!/usr/bin/env bash
# Raspberry Pi 4 の SD カードドライバ (EMMC2) の確認。
#
# ---- ★ QEMU と実機で配線が違う -------------------------------------------
#
# **QEMU の raspi4b は SD カードを旧 sdhci (0xFE300000) に繋いでいて、
# EMMC2 (0xFE340000) は空のまま。** hw/arm/bcm2838_peripherals.c が
# GPIO の "sdbus-sdhci" を s_base->sdhci に結んでいるため。
# 実機の Pi 4 は逆で、SD カードは EMMC2、0xFE300000 は WiFi の SDIO。
#
# **どちらも QEMU では同じ generic-sdhci モデル**なので、番地だけ差し替えれば
# ドライバの中身 (初期化手順 / PIO 転送 / CSD の容量 / LBA の単位) は
# 確かめられる。ここでやっているのはそれ。
#
#   AARCH64_EMMC2_BASE=0xFE300000   ← **検証専用。実機向けには渡さない**
#
# したがってこのスモークが見ているのは **ドライバの中身**であって、
# 「実機の SD カードが読める」ことではない。実機での確認は未了。
set -euo pipefail
cd "$(dirname "$0")/.."

LOG=LOGs/aarch64-pi4-sd.log
IMG=out/kernel8.img
SD=out/aarch64-pi4-sd.img
SDFS=out/aarch64-pi4-sdfs

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
    echo "aarch64 pi4 sd smoke test: SKIP (raspi4b を持つ qemu が無い)"
    exit 0
fi

# SD カードの中身。**xv6fs を載せて、既知のファイルを中身まで照合させる。**
# 「マウントできた」だけでは、別のブロックを返していても気づけない
rm -rf "$SDFS"; mkdir -p "$SDFS/bin"
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$SDFS/aarch64-m4.txt"
cp out/aarch64-hello.elf "$SDFS/bin/hello"
rm -f "$SD"
XV6FS_FSSIZE=8192 XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$SDFS" "$SD" > /dev/null

mkdir -p LOGs
rm -f "$LOG"

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

"$QEMU_BIN" -machine raspi4b -nographic -kernel "$IMG" \
    -dtb tests/dtb/bcm2711-rpi-4-b.dtb \
    -drive file="$SD",if=sd,format=raw \
    < /dev/null > "$LOG" 2>&1 &
QEMU_PID=$!
# **待ちには上限を付ける。** PIO なので virtio より遅い
for _ in {1..90}; do
    if grep -aqE "aarch64-fs-(ok|BAD|none)" "$LOG" 2>/dev/null; then break; fi
    sleep 1
done
# P1 の exec まで見たいので、少しだけ余分に待つ
for _ in {1..20}; do
    if grep -aqE "aarch64-init-(ok|BAD)|bootstrap user exit" "$LOG" 2>/dev/null; then break; fi
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- Raspberry Pi 4 SD (EMMC2 ドライバ) Serial Output ---"
cat "$LOG"
echo "----------------------------------"

must_not() {
    if grep -aq "$1" "$LOG"; then
        echo "FAIL: 出てはいけない文字列があった: $1"
        exit 1
    fi
}

# ---- 初期化 ----
# **「見つかった」だけでは足りない。** 容量まで出させる。
# CSD の 8 ビットずれを取り違えると桁が変わるので、ここで捕まる
grep -aq "  emmc2     : 初期化 ok" "$LOG"
# 8MB のイメージ = 0x4000 ブロック。**期待値を書いて照合する**
grep -aq "blocks=0x0000000000004000" "$LOG"
must_not "emmc2     : カードが無い"
must_not "emmc2     : 容量が 0 と出た"

# ---- storage / xv6fs ----
# **sd0 として登録され、xv6fs がその上に載ること**
grep -aq "xv6fs: mounted sd0" "$LOG"
grep -aq "  mount     :  ok" "$LOG"
grep -aq "  root=xv6fs:  ok" "$LOG"
# 中身まで照合。読めた内容が正しいことの証拠
grep -aq "  read file :  ok" "$LOG"
# 書いて読み戻す。**PIO の書き込み側が効いている証拠**
grep -aq "  write     :  ok" "$LOG"
grep -aq "  read back :  ok" "$LOG"
grep -aq "  fd read   :  ok" "$LOG"
grep -aq "aarch64-fs-ok" "$LOG"
must_not "aarch64-fs-BAD"
must_not "xv6bio: disk"

# ---- P1 ----
# **ELF を SD から読んで EL0 で走らせるところまで。**
# ここが通れば、ドライバ -> storage -> xv6bio -> xv6fs -> VFS -> exec が
# 全部繋がっている
grep -aq "\[EL0\] hello from aarch64 userland via svc" "$LOG"
must_not "aarch64-init-BAD"
must_not "aarch64-exception-BAD"

echo "aarch64 pi4 sd smoke test: PASS (EMMC2 初期化 / 容量 / 読み書き / SD から exec)"
echo "  ※ QEMU は SD を旧 sdhci に繋ぐため番地を差し替えて確認している。"
echo "    実機 (EMMC2 0xFE340000) での確認は未了"
