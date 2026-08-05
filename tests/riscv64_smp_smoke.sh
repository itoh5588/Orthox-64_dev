#!/bin/bash
# RISC-V SMP 検証: -smp 4 で起動し、SBI HSM で副 hart が idle まで上がった上で
# ユーザーランド (preempt probe) が完走することを確認する。
#   make riscv64-smp-smoke がビルドとあわせて実行する
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
elif [ -f /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    # Debian/Ubuntu の qemu-system-misc はここに置く
    FW_PATH=/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SERIAL_LOG=LOGs/riscv64-smp-serial.log
rm -f "$SERIAL_LOG"

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 4 \
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

echo "--- RISC-V SMP Serial Output ---"
cat "$SERIAL_LOG"
echo "------------------------------------"

grep -q "\[smp\] cpus detected: 0x0000000000000004" "$SERIAL_LOG"
# 副 hart 3 本が idle まで到達している
grep -q "\[smp\] cpu online: 0x0000000000000001" "$SERIAL_LOG"
grep -q "\[smp\] cpu online: 0x0000000000000002" "$SERIAL_LOG"
grep -q "\[smp\] cpu online: 0x0000000000000003" "$SERIAL_LOG"
grep -q "\[smp\] cpus online: 0x0000000000000004" "$SERIAL_LOG"
# マルチ hart でもユーザーランドが完走する
grep -q "PREEMPT-OK" "$SERIAL_LOG"

echo "riscv64 SMP smoke test: PASS"
