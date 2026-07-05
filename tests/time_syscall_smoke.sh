#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/time-syscall-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/time-syscall-qemu.out}"
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
/bin/testtime.elf
EOF

make orthos.iso >/tmp/time-syscall-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for time syscall smoke..."
for _ in $(seq 1 60); do
    if grep -q 'tick2=' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

grep -q -- '--- Orthox-64 v0.3.0 Boot ---' "${SERIAL_LOG}"
grep -q 'tick0=' "${SERIAL_LOG}"
grep -q 'tick1=' "${SERIAL_LOG}"
grep -q 'tick2=' "${SERIAL_LOG}"

echo "time syscall smoke PASS"
tail -n 120 "${SERIAL_LOG}"
