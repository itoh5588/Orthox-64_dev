#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-kernel-build-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-kernel-build-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_kernel_build_smoke.sh"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
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
    if [ -f "${SCRIPT_BACKUP}" ]; then
        if [ -s "${SCRIPT_BACKUP}" ]; then
            cp "${SCRIPT_BACKUP}" "${SCRIPT_PATH}"
        else
            rm -f "${SCRIPT_PATH}"
        fi
        rm -f "${SCRIPT_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
else
    : > "${SCRIPT_BACKUP}"
fi

cat > "${SCRIPT_PATH}" <<'EOF'
export PATH=/bin:/usr/bin:/
echo native-kernel-build-start
mkdir -p /kbuild/kernel /kbuild/lwip/core/ipv4 /kbuild/lwip/netif
make -f /src/kernel-build/Makefile BUILD=/kbuild OUTPUT=/kernel.elf
echo native-kernel-build-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_kernel_build_smoke.sh
EOF

echo "Building ${ISO}..."
make "${ISO}" > /tmp/native-kernel-build-build.out 2>&1

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

echo "Running QEMU (timeout 3600s)..."
qemu-system-x86_64 "${QEMU_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 3600); do
    if grep -q "kernel-native-build: PASS" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "EXCEPTION OCCURRED\|#PF(User):\|#UD" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if grep -q "kernel-native-build: PASS" "${SERIAL_LOG}" 2>/dev/null; then
    echo "Test passed: kernel built natively on Orthox-64."
    exit 0
fi

echo "Test failed. Serial log (tail):"
tail -n 100 "${SERIAL_LOG}"
exit 1
