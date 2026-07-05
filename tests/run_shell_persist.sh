#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

MAKE_ARGS=()
QEMU_ARGS=()
BOOTCMD_PATH="rootfs/etc/bootcmd"
BOOTCMD_BACKUP=""

cleanup() {
    if [ -n "$BOOTCMD_BACKUP" ] && [ -f "$BOOTCMD_BACKUP" ]; then
        cp "$BOOTCMD_BACKUP" "$BOOTCMD_PATH"
        rm -f "$BOOTCMD_BACKUP"
    fi
}
trap cleanup EXIT

while [ "$#" -gt 0 ]; do
    case "$1" in
        --rebuild-rootfs)
            MAKE_ARGS+=("ROOTFS_REBUILD=1")
            shift
            ;;
        --)
            shift
            while [ "$#" -gt 0 ]; do
                QEMU_ARGS+=("$1")
                shift
            done
            ;;
        *)
            QEMU_ARGS+=("$1")
            shift
            ;;
    esac
done

if [ "${#MAKE_ARGS[@]}" -eq 0 ]; then
    MAKE_ARGS+=("ROOTFS_REBUILD=0")
fi

if [ -f "$BOOTCMD_PATH" ]; then
    BOOTCMD_BACKUP="$(mktemp)"
    cp "$BOOTCMD_PATH" "$BOOTCMD_BACKUP"
    : > "$BOOTCMD_PATH"
fi

make "${MAKE_ARGS[@]}" orthos.iso rootfs.img

exec bash ./run_qemu_stdio.sh \
    -drive if=none,id=rootfs,file=rootfs.img,format=raw \
    -device virtio-blk-pci,drive=rootfs \
    "${QEMU_ARGS[@]}"
