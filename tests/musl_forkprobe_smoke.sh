#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-forkprobe-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-forkprobe-qemu.out}"
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
/bin/muslforkprobe.elf
EOF

make orthos.iso >/tmp/musl-forkprobe-build.out

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

echo "Waiting for musl fork probe..."
for _ in $(seq 1 60); do
    if grep -q 'muslforkprobe:ok' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

grep -q 'muslforkprobe:start' "${SERIAL_LOG}"
grep -q 'muslforkprobe:child' "${SERIAL_LOG}"
grep -q 'muslforkprobe:parent child=' "${SERIAL_LOG}"
grep -q 'muslforkprobe:status=42' "${SERIAL_LOG}"
grep -q 'muslforkprobe:ok' "${SERIAL_LOG}"

echo "musl fork probe PASS"
tail -n 120 "${SERIAL_LOG}"
