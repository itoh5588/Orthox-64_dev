#!/bin/bash
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

SERIAL_LOG=LOGs/riscv64-serial.log
rm -f "$SERIAL_LOG"

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 1 \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -monitor none &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..20}; do
    if grep -q "Orthox riscv64 early boot" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

sleep 2

echo "--- RISC-V Serial Output ---"
cat "$SERIAL_LOG"
echo "----------------------------"

grep -q "Orthox riscv64 early boot" "$SERIAL_LOG"
grep -q "dtb size: 0x" "$SERIAL_LOG"
grep -q "hart: 0x0000000000000000" "$SERIAL_LOG"
grep -q "uart   : 0x0000000010000000" "$SERIAL_LOG"
grep -q "virtio0: 0x" "$SERIAL_LOG"
grep -q "sv39 satp enabled" "$SERIAL_LOG"
grep -q "vm selftest passed" "$SERIAL_LOG"
grep -q "vm clone selftest passed" "$SERIAL_LOG"
grep -q "user stack selftest passed" "$SERIAL_LOG"
grep -q "user frame selftest passed" "$SERIAL_LOG"
grep -q "user frame sync selftest passed" "$SERIAL_LOG"
grep -q "trap vector installed" "$SERIAL_LOG"
grep -q "sbi timer armed" "$SERIAL_LOG"
grep -q "Task system initialized." "$SERIAL_LOG"
grep -q "syscall dispatch selftest passed" "$SERIAL_LOG"
grep -q "user map selftest passed" "$SERIAL_LOG"
grep -q "fork selftest passed" "$SERIAL_LOG"
grep -q "first user task entering user mode" "$SERIAL_LOG"
grep -q "ELF Segment loaded: Virt 0x" "$SERIAL_LOG"
grep -q "SOK" "$SERIAL_LOG"
grep -q "C/" "$SERIAL_LOG"
grep -q "bootstrap user exit" "$SERIAL_LOG"

USER_ROOT="$(awk '/enter user ctx root:/ {print $NF; exit}' "$SERIAL_LOG")"
KERNEL_ROOT="$(awk '/enter user kernel :/ {print $NF; exit}' "$SERIAL_LOG")"
if [ -z "$USER_ROOT" ] || [ -z "$KERNEL_ROOT" ] || [ "$USER_ROOT" = "$KERNEL_ROOT" ]; then
    echo "user address space did not diverge from kernel root" >&2
    exit 1
fi

echo "riscv64 smoke test: PASS"
