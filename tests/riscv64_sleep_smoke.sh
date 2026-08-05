#!/bin/bash
# nanosleep のタイマー起床経路の検証。
# riscv64_sleep_probe が 200ms x 5 回眠り、実測経過を報告する。
# 起床経路が死んでいると戻ってこない (タイムアウト)、寝ていなければ経過が短い。
#   make riscv64-sleep-smoke がビルドとあわせて実行する
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

SMP_CPUS="${SMP_CPUS:-1}"
SERIAL_LOG=LOGs/riscv64-sleep-serial.log
rm -f "$SERIAL_LOG"

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp "$SMP_CPUS" \
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
    if grep -aq "SLEEP-PROBE-OK\|SLEEP-PROBE-BAD" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V sleep Serial Output ---"
cat "$SERIAL_LOG"
echo "----------------------------------"

grep -aq "SLEEP-PROBE-START" "$SERIAL_LOG"
# 5 回とも nanosleep から戻れた = 起床経路が生きている
[ "$(grep -ac "SLEEP-TICK" "$SERIAL_LOG")" = "5" ]
# 子が眠り親が wait する形 (busybox の `sleep 1` と同じ) でも起きられる
grep -aq "SLEEP-CHILD-OK" "$SERIAL_LOG"
grep -aq "SLEEP-PROBE-OK" "$SERIAL_LOG"

echo "riscv64 sleep smoke test: PASS"
