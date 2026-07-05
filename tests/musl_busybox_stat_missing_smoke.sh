#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-busybox-stat-missing-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-busybox-stat-missing-qemu.out}"
DRIVER_ELF="user/muslbusyboxstatdriver.elf"
SH_ELF_PATH="user/sh.elf"
SH_ELF_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${SH_ELF_BACKUP}" ]; then
        if [ -s "${SH_ELF_BACKUP}" ]; then
            cp "${SH_ELF_BACKUP}" "${SH_ELF_PATH}"
        fi
        rm -f "${SH_ELF_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${SH_ELF_PATH}" "${SH_ELF_BACKUP}"

make "${DRIVER_ELF}" >/tmp/musl-busybox-stat-missing-driver-build.out
cp "${DRIVER_ELF}" "${SH_ELF_PATH}"

make orthos.iso >/tmp/musl-busybox-stat-missing-build.out

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

echo "Waiting for musl BusyBox stat-missing smoke..."
for _ in $(seq 1 90); do
    if grep -q 'muslbusyboxstatdriver:status=' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "can't stat '/definitely-missing'" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q '\*\*\* EXCEPTION OCCURRED \*\*\*' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q '#PF(User):' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

echo "--- Serial Tail ---"
tail -n 200 "${SERIAL_LOG}"
