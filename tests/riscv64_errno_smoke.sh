#!/bin/bash
# 失敗系 syscall の errno 検証。
# riscv64_errno_probe が失敗するはずの呼び出しを並べ、返る errno を報告する。
# カーネルが -1 を返していると EPERM として顕在化する (busybox の `rm -f` が壊れる)。
#   make riscv64-errno-smoke がビルドとあわせて実行する
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
SERIAL_LOG=LOGs/riscv64-errno-serial.log
ROOTFS_IMG=out/rootfs-riscv64-xv6.img
rm -f "$SERIAL_LOG"

# /etc を触るケースがあるので rootfs があれば繋ぐ
VBLK_ARGS=()
if [ -f "$ROOTFS_IMG" ]; then
    VBLK_ARGS=(-drive "if=none,id=rootfs,file=$ROOTFS_IMG,format=raw"
               -device virtio-blk-device,drive=rootfs)
fi

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp "$SMP_CPUS" \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    "${VBLK_ARGS[@]}" \
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
    if grep -aq "ERRNO-PROBE-OK\|ERRNO-PROBE-BAD" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V errno Serial Output ---"
cat "$SERIAL_LOG"
echo "----------------------------------"

grep -aq "ERRNO-PROBE-START" "$SERIAL_LOG"
# 1 件でも期待と違えば probe 側が BAD を出す
! grep -aq " BAD" "$SERIAL_LOG"
grep -aq "ERRNO-PROBE-OK" "$SERIAL_LOG"

echo "riscv64 errno smoke test: PASS"
