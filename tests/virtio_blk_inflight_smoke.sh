#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
BOOTCMD_BACKUP="$(mktemp)"
WRITE_LOG="${WRITE_LOG:-LOGs/virtio-blk-inflight-write.log}"
mkdir -p LOGs

cleanup() {
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        rm -f "${BOOTCMD_BACKUP}"
    fi
}
trap cleanup EXIT

if [ -f "${BOOTCMD_PATH}" ]; then
    cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
else
    : > "${BOOTCMD_BACKUP}"
fi

: > "${BOOTCMD_PATH}"
make orthos.iso >/tmp/virtio-blk-inflight-build.out

rm -f "${WRITE_LOG}"

(
    sleep 2
    echo "vblk_test"
    sleep 1
    echo "vblk_test read"
    sleep 3
) | timeout 45s qemu-system-x86_64 \
    -machine pc \
    -cpu max \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -device virtio-blk-pci,drive=hd0,disable-modern=on \
    -drive id=hd0,file=rootfs.img,format=raw,if=none \
    -serial stdio >"${WRITE_LOG}" 2>&1 || true

grep -q 'reqs=0x0000000000000008' "${WRITE_LOG}"
grep -qE '\[boot\] mounted xv6fs root image on (vblk0|bootimg0)' "${WRITE_LOG}"
grep -q 'Write successful' "${WRITE_LOG}"
grep -q 'SUCCESS: Persistence verified!' "${WRITE_LOG}"

echo "VirtIO Block inflight smoke PASS"
tail -n 120 "${WRITE_LOG}"
