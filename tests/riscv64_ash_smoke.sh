#!/bin/bash
# busybox ash を対話シェルとして起動し、シリアル (stdio) 経由でコマンドを
# 流し込んで応答を検証する。実行前に以下でカーネルをビルドしておくこと:
#   make riscv64-kernel RISCV64_BOOTSTRAP_USER_SRC_ELF=out/busybox-riscv64-musl.elf RISCV64_BOOTSTRAP_ARG0_VALUE=sh
# (make riscv64-ash-smoke がこの手順をまとめて行う)
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

SERIAL_LOG=LOGs/riscv64-ash-serial.log
rm -f "$SERIAL_LOG"

(
    sleep 8
    printf 'echo interactive-ok\n'
    sleep 2
    printf 'pwd\n'
    sleep 2
    printf 'x=42; echo val=$x\n'
    sleep 2
    printf 'exit\n'
    sleep 3
) | "$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 1 \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial stdio \
    -monitor none > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    pkill -f qemu-system-riscv64 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..30}; do
    if grep -q "bootstrap user exit" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V ash Serial Output ---"
cat "$SERIAL_LOG"
echo "--------------------------------"

grep -q "built-in shell (ash)" "$SERIAL_LOG"
grep -q "riscv64 supervisor timer interrupt" "$SERIAL_LOG"
grep -q "interactive-ok" "$SERIAL_LOG"
grep -q "^/" "$SERIAL_LOG"
grep -q "val=42" "$SERIAL_LOG"
grep -q "bootstrap user exit" "$SERIAL_LOG"

echo "riscv64 ash smoke test: PASS"
