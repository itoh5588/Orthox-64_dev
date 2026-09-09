#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
ROOTFS_IMG="${ROOTFS_IMG:-rootfs.img}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/xv6-reclaim-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/xv6-reclaim-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
BOOTCMD_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        if [ -f "${ROOTFS_IMG}" ]; then
            python3 scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd "${BOOTCMD_BACKUP}" "${ROOTFS_IMG}" >/dev/null || true
        fi
        rm -f "${BOOTCMD_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/xv6_reclaim_test.elf
EOF

make "${ISO}" >/tmp/xv6-reclaim-build.out

python3 scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd "${BOOTCMD_PATH}" "${ROOTFS_IMG}" >/dev/null

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -drive if=none,id=rootfs,file="${ROOTFS_IMG}",format=raw \
    -device virtio-blk-pci,drive=rootfs \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for xv6 reclaim smoke..."
for _ in $(seq 1 120); do
    if grep -q 'xv6-reclaim-smoke: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'xv6-reclaim-smoke: PASS' "${SERIAL_LOG}"; then
    echo "xv6 reclaim smoke FAIL: pass marker not found"
    tail -n 160 "${SERIAL_LOG}"
    exit 1
fi

python3 scripts/build_rootfs_xv6fs.py --check "${ROOTFS_IMG}"

echo "xv6 reclaim smoke PASS"
tail -n 160 "${SERIAL_LOG}"
