#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_PATH="$SCRIPT_DIR/out/kernel-riscv64.elf"
QEMU_BIN="$(command -v qemu-system-riscv64 2>/dev/null || true)"
FW_PATH=""

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

if [ -f /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
elif [ -f /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

ROOTFS_IMG="$SCRIPT_DIR/out/rootfs-riscv64-xv6.img"
DRIVE_ARGS=()
if [ -f "$ROOTFS_IMG" ]; then
    DRIVE_ARGS=(-drive "file=$ROOTFS_IMG,if=none,format=raw,id=vblk0" -device virtio-blk-device,drive=vblk0)
fi

exec "$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 1 \
    -bios "$FW_PATH" \
    -kernel "$KERNEL_PATH" \
    -display none \
    -serial mon:stdio \
    -monitor none \
    "${DRIVE_ARGS[@]}" \
    "$@"
