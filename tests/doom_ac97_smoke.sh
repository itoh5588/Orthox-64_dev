#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/doom-ac97-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/doom-ac97-qemu.out}"
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
{
    cat "${BOOTCMD_BACKUP}"
    echo "/bin/doom-musl.elf"
} > "${BOOTCMD_PATH}"

make orthos.iso ORTHOS_LIBGCC=/usr/lib/gcc/x86_64-linux-gnu/13/libgcc.a >/tmp/doom-ac97-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 2G \
    -bios Web/wasabi/third_party/ovmf/RELEASEX64_OVMF.fd \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audiodev none,id=audio0 \
    -device AC97,audiodev=audio0 \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for Doom AC97 boot smoke..."
for _ in $(seq 1 60); do
    if grep -q '\[doomsound\] init probe=' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'orthOS DOOM build' "${SERIAL_LOG}" 2>/dev/null; then
    echo "Doom did not start"
    tail -n 200 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

if ! grep -q '\[doomsound\] init' "${SERIAL_LOG}" 2>/dev/null; then
    echo "Doom sound module did not initialize"
    tail -n 200 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

PROBE_LINE="$(grep '\[doomsound\] init probe=' "${SERIAL_LOG}" | tail -n 1)"
PROBE_VALUE="${PROBE_LINE##*=}"
PROBE_VALUE="${PROBE_VALUE%%[!0-9-]*}"

if [ -z "${PROBE_VALUE}" ] || [ "${PROBE_VALUE}" -le 0 ]; then
    echo "Doom sound init probe failed: ${PROBE_LINE}"
    tail -n 200 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

echo "Doom AC97 smoke PASS: ${PROBE_LINE}"
tail -n 180 "${SERIAL_LOG}"
