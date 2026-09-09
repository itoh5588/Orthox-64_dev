#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/dynlink-busybox-ash-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/dynlink-busybox-ash-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/dynlink_busybox_ash.ash"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
SCRIPT_HAD_FILE=0
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        rm -f "${BOOTCMD_BACKUP}"
    fi
    if [ "${SCRIPT_HAD_FILE}" = "1" ]; then
        cp "${SCRIPT_BACKUP}" "${SCRIPT_PATH}"
    else
        rm -f "${SCRIPT_PATH}"
    fi
    rm -f "${SCRIPT_BACKUP}"
}
trap cleanup EXIT

mkdir -p "$(dirname "${BOOTCMD_PATH}")" "$(dirname "${SCRIPT_PATH}")"
cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
    SCRIPT_HAD_FILE=1
fi

cat > "${SCRIPT_PATH}" <<'EOF'
echo dynlink-ash-start
echo dynlink-ash-middle
echo dynlink-ash-pass
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/busybox.dyn ash /etc/dynlink_busybox_ash.ash
EOF

echo "Building ${ISO}..."
make "${ISO}" > /tmp/dynlink-busybox-ash-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

QEMU_ARGS=(
    -M q35
    -cpu max
    -m 2G
    -cdrom "${ISO}"
    -boot d
    -display none
    -serial "file:${SERIAL_LOG}"
    -no-reboot
    -drive if=none,id=rootfs,file=rootfs.img,format=raw
    -device virtio-blk-pci,drive=rootfs
)

echo "Running QEMU..."
qemu-system-x86_64 "${QEMU_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 180); do
    if grep -q "dynlink-ash-pass" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "EXCEPTION OCCURRED" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "#PF(User):" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if grep -q "dynlink-ash-pass" "${SERIAL_LOG}" && ! grep -q "EXCEPTION OCCURRED" "${SERIAL_LOG}" && ! grep -q "#PF(User):" "${SERIAL_LOG}"; then
    echo "Test passed: dynamic BusyBox ash script works."
    exit 0
fi

echo "Test failed. Serial log follows:"
cat "${SERIAL_LOG}"
exit 1
