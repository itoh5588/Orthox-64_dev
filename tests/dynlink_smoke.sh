#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/dynlink-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/dynlink-qemu.out}"
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
        rm -f "${BOOTCMD_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/hello_dyn.elf
/bin/dynlink_multi_tls.elf
/bin/dynlink_dlopen.elf
EOF

echo "Building ${ISO}..."
make ${ISO} > /dev/null

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

for _ in $(seq 1 120); do
    if grep -q "Hello, Orthox-64 with Shared Library!" "${SERIAL_LOG}" 2>/dev/null && \
       grep -q "dynlink-multi-tls: PASS" "${SERIAL_LOG}" 2>/dev/null && \
       grep -q "dynlink-dlopen: PASS" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "Exec: Interpreter not found" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "dynlink-multi-tls: FAIL" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "dynlink-dlopen: FAIL" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "Page Fault" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

echo "Checking serial log for success..."
if grep -q "Hello, Orthox-64 with Shared Library!" "${SERIAL_LOG}" && \
   grep -q "dynlink-multi-tls: PASS" "${SERIAL_LOG}" && \
   grep -q "dynlink-dlopen: PASS" "${SERIAL_LOG}"; then
    echo "Test passed: Dynamic linking works."
    exit 0
else
    echo "Test failed. Serial log follows:"
    cat "${SERIAL_LOG}"
    exit 1
fi
