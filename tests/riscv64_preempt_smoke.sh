#!/bin/bash
# ユーザーモードのタイマープリエンプション検証。
# riscv64_preempt_probe が CPU を占有し続ける子を作った上で、親が read から
# 復帰できることを確認する。プリエンプションが無いと親は永久に走れない。
#   make riscv64-preempt-smoke がビルドとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-riscv64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-riscv64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-riscv64
fi
if [ -z "$QEMU_BIN" ] && [ -x /usr/local/bin/qemu-system-riscv64 ]; then
    QEMU_BIN=/usr/local/bin/qemu-system-riscv64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-riscv64 not found" >&2
    exit 1
fi

FW_PATH=""
if [ -f /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
elif [ -f /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SERIAL_LOG=LOGs/riscv64-preempt-serial.log
rm -f "$SERIAL_LOG"

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 1 \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial stdio \
    -monitor none < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    pkill -f qemu-system-riscv64 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..40}; do
    if grep -q "PREEMPT-OK" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V preempt Serial Output ---"
cat "$SERIAL_LOG"
echo "------------------------------------"

grep -q "PARENT-WAIT" "$SERIAL_LOG"
# CPU を手放さない子がいる状態で親が復帰できた = プリエンプションが効いている
grep -q "PREEMPT-OK" "$SERIAL_LOG"

echo "riscv64 preempt smoke test: PASS"
