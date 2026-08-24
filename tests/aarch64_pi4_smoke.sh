#!/usr/bin/env bash
# Raspberry Pi 4 (BCM2711) 向けの起動確認。**絶対目的は実機での動作**なので、
# ここは実機に行くまでの代わりであって、実機の代わりにはならない。
#
# 何を見るか:
#   - 生イメージ kernel8.img が 0x80000 から起動すること
#   - **実物の Pi 4 の DTB** を解釈して UART / GIC / CPU 数を取り出すこと
#   - Pi の周辺 (PL011 0xFE201000 / GIC-400 0xFF841000) で最後まで走ること
#   - **GIC-400 でタイマ割り込みが届くこと** (sleep が起きる)
#
# 何を見られないか (実機でしか確かめられない。tests/dtb/README.md も参照):
#   - RAM の本当の大きさ。配布 DTB の /memory@0 は reg = <0 0 0> で、
#     実機ではファームウェアが起動時に書き換える
#   - EMMC2 (SD カード) / DMA のキャッシュコヒーレンシ
#   - シリアルの配線と config.txt の効き方
#
# **raspi4b は QEMU 9.0 以降**。Ubuntu 24.04 の apt は 8.2 系までなので、
# 手元では $HOME/qemu-build/build に自前ビルドしたものを使っている。
# 見つからなければ skip する (失敗にしない) — 環境が揃っていないことと、
# カーネルが壊れていることを混ぜない。
set -euo pipefail
cd "$(dirname "$0")/.."

# **QEMU 用のイメージを既定にできるよう外から差せる。** 実機向けの
# out/pi4-boot/kernel8.img は AARCH64_SOUND=1 で組まれていて、
# QEMU の raspi4b では PWM1 が無く落ちる (Makefile の注記)
IMG="${PI4_IMG:-out/pi4-boot/kernel8.img}"
DTB=tests/dtb/bcm2711-rpi-4-b.dtb
LOG=LOGs/aarch64-pi4.log

# raspi4b を持っている qemu を探す。**--version では分からない**ので
# -machine help で機種名を確かめる
QEMU_BIN=""
for cand in "$HOME/qemu-build/build/qemu-system-aarch64" \
            "$HOME/qemu-local/bin/qemu-system-aarch64" \
            "$(command -v qemu-system-aarch64 2>/dev/null || true)"; do
    [ -n "$cand" ] && [ -x "$cand" ] || continue
    if "$cand" -machine help 2>/dev/null | grep -q "^raspi4b "; then
        QEMU_BIN="$cand"
        break
    fi
done
if [ -z "$QEMU_BIN" ]; then
    echo "aarch64 pi4 smoke test: SKIP (raspi4b を持つ qemu が無い。QEMU 9.0 以降が要る)"
    exit 0
fi
echo "qemu: $QEMU_BIN ($("$QEMU_BIN" --version | head -1))"

[ -f "$IMG" ] || { echo "$IMG が無い。make aarch64-pi4-boot を先に実行すること"; exit 1; }
[ -f "$DTB" ] || { echo "$DTB が無い"; exit 1; }

mkdir -p LOGs
rm -f "$LOG"

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

# **-dtb は必須。** 付けないと x0 に 0x100 が入り DTB が渡らない
# (raspi3b でも同じ)。実機はファームウェアが bcm2711-rpi-4-b.dtb を
# 読んで渡すので、付けるほうが実機に近い
"$QEMU_BIN" -machine raspi4b -nographic -kernel "$IMG" -dtb "$DTB" \
    < /dev/null > "$LOG" 2>&1 &
QEMU_PID=$!
# **待ちには上限を付ける** (日報2026-08-11 追§9)
for _ in {1..60}; do
    if grep -aqE "aarch64-fs-(ok|BAD|none)" "$LOG" 2>/dev/null; then break; fi
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- Raspberry Pi 4 (raspi4b) Serial Output ---"
cat "$LOG"
echo "----------------------------------"

must_not() {  # $1 = 出てはいけない文字列
    if grep -aq "$1" "$LOG"; then
        echo "FAIL: 出てはいけない文字列があった: $1"
        exit 1
    fi
}

# ---- 起動形式 ----
# 生イメージが 0x80000 から動いた証拠。ここが出なければ以降は見るまでもない
grep -aq "aarch64-boot-ok" "$LOG"
# **入口の EL。** armstub と同じく EL2 で来る。EL1 と出たら
# 「降格路を通っていない」ので、実機との条件が変わっている
grep -aq "(入口 EL2)" "$LOG"

# ---- D: DTB (実物の Pi 4 の木) ----
# **どれも QEMU virt とは違う値。** 既定値のままなら (dtb) が付かない
grep -aq "  uart      : 0x00000000fe201000  (dtb)" "$LOG"
grep -aq "  gic dist  : 0x00000000ff841000" "$LOG"
grep -aq "  gic cpu   : 0x00000000ff842000" "$LOG"
# ranges を変換していないと 0x7e201000 のまま = どこにも繋がらず沈黙する
must_not "0x000000007e201000"
# status = disabled のポートを掴んでいないこと (無効な 0xfe201a00 を選ぶと沈黙)
must_not "0x00000000fe201a00"
# 4 コア。1 と出たら cpu ノードの数え方が壊れている
grep -aq "  cpus      : 0x0000000000000004" "$LOG"
grep -aq "aarch64-dtb-ok" "$LOG"

# ---- Pi には virtio が無い ----
# **virt の既定値 (0x0a000000) を掴んでいないこと。** 掴むと RAM のブロックと
# 重なって MMU のテーブル構築ごと落ちる
grep -aq "  virtio    : 0x0000000000000000" "$LOG"
grep -aq "aarch64-virtio-none" "$LOG"

# ---- F: GIC-400 ----
# **タイマ割り込みが届いていること。** GIC-400 は GICv2 なので既存の実装で
# 通る見込みだった、を実測に変える。ticks が進み、sleep が起きること
grep -aq "aarch64-timer-ok" "$LOG"
grep -aq "sleep     : .* ok (寝て、タイマに起こされた)" "$LOG"
grep -aq "aarch64-sched-ok" "$LOG"

# ---- MMU と EL0 ----
grep -aq "aarch64-mmu-ok" "$LOG"
grep -aq "aarch64-user-ok" "$LOG"
must_not "aarch64-mmu-BAD"
must_not "aarch64-exception-BAD"
# ストレージが無いので fs は none になる。**「無い」と「壊れた」を混ぜない**
grep -aq "aarch64-fs-none" "$LOG"
must_not "aarch64-fs-BAD"

echo "aarch64 pi4 smoke test: PASS (起動形式 / DTB / GIC-400 / MMU / EL0)"
echo "  ※ ストレージ (EMMC2) と DMA コヒーレンシは未実装。実機の確認も未了"
