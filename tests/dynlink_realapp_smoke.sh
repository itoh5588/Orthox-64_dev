#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/dynlink-realapp-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/dynlink-realapp-qemu.out}"
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

mkdir -p "$(dirname "${BOOTCMD_PATH}")"
cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/dynlink_malloc.elf
/bin/busybox.dyn echo dyn-busybox-echo
/bin/busybox.dyn cat /hello.txt
/bin/busybox.dyn ls /bin
/bin/busybox.dyn wc -l /hello.txt
/bin/gcc.dyn -S /test_native.c -o /tmp/dyn_toolchain.s
/bin/busybox.dyn test -f /tmp/dyn_toolchain.s
/bin/busybox.dyn echo dynlink-realapp-toolchain-asm-ok
/bin/gcc.dyn -c /test_native.c -o /tmp/dyn_toolchain.o
/bin/busybox.dyn test -f /tmp/dyn_toolchain.o
/bin/busybox.dyn echo dynlink-realapp-toolchain-obj-ok
/bin/busybox.dyn echo dynlink-realapp-pass
EOF

echo "Building ${ISO}..."
make "${ISO}" > /tmp/dynlink-realapp-build.out

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
    if grep -q "dynlink-realapp-pass" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "dynlink-malloc: FAIL" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "Exec: Interpreter not found" "${SERIAL_LOG}" 2>/dev/null; then
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

if grep -q "dynlink-malloc: PASS" "${SERIAL_LOG}" && \
   grep -q "dynlink-realapp-pass" "${SERIAL_LOG}" && \
   grep -q "dynlink-realapp-toolchain-asm-ok" "${SERIAL_LOG}" && \
   grep -q "dynlink-realapp-toolchain-obj-ok" "${SERIAL_LOG}"; then
    echo "Test passed: dynamic BusyBox and dynamic toolchain wrapper work."
    exit 0
fi

echo "Test failed. Serial log follows:"
cat "${SERIAL_LOG}"
exit 1
