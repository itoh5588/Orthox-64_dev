#!/bin/bash
# AArch64 (QEMU virt) の起動スモーク。M0: PL011 に文字が出るところまで。
#
# 日報2026-08-02 の段取り 1 の最初の一歩。ツールチェーン (clang の aarch64
# ターゲット) / リンカスクリプト / QEMU の起動 / シリアル経路を一度に見る。
#
# 判定は「動いた」だけにしない。EL / MPIDR / DTB / bss ゼロ埋めを表示させ、
# 中身が妥当かまで見る (起動できても bss が埋まっていなければ、この先の C が
# 静かに壊れる)。
#
#   make aarch64-smoke がビルドとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-aarch64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-aarch64 not found" >&2
    exit 1
fi

KERNEL=out/kernel-aarch64.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-kernel')" >&2; exit 1; }

SERIAL_LOG=LOGs/aarch64-serial.log
rm -f "$SERIAL_LOG"

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

"$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp 1 \
    -nographic \
    -kernel "$KERNEL" < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..30}; do
    if grep -aq "aarch64-boot-ok" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- AArch64 Serial Output ---"
cat "$SERIAL_LOG"
echo "-----------------------------"

grep -aq "Orthox-64 aarch64 boot" "$SERIAL_LOG"
grep -aqE "CurrentEL : EL[12]" "$SERIAL_LOG"   # virt は EL1、virtualization=on なら EL2
grep -aq "bss zero  : ok" "$SERIAL_LOG"        # start.S のゼロ埋めが効いていること
! grep -aq "bss zero  : BAD" "$SERIAL_LOG"
grep -aq "aarch64-boot-ok" "$SERIAL_LOG"

echo "aarch64 smoke test: PASS"
