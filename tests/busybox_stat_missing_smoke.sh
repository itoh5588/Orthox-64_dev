#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/busybox-stat-missing-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/busybox-stat-missing-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/busybox_stat_missing.ash"
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
echo stat-missing-start
/bin/staterrno.elf
echo stat-missing-after-staterrno
stat /definitely-missing
echo stat-missing-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/busybox_stat_missing.ash
EOF

make orthos.iso >/tmp/busybox-stat-missing-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for BusyBox stat missing smoke..."
for _ in $(seq 1 90); do
    if grep -q 'stat-missing-end' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q '\*\*\* EXCEPTION OCCURRED \*\*\*' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

echo "--- Serial Tail ---"
tail -n 200 "${SERIAL_LOG}"
