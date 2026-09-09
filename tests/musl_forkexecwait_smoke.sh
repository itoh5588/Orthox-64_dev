#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-forkexecwait-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-forkexecwait-qemu.out}"
DRIVER_ELF="user/muslforkexecdriver.elf"
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

make "${DRIVER_ELF}" >/tmp/musl-forkexecwait-driver-build.out
cp "${DRIVER_ELF}" "${SH_ELF_PATH}"

make orthos.iso >/tmp/musl-forkexecwait-build.out

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

echo "Waiting for musl fork+exec+wait smoke..."
for _ in $(seq 1 90); do
    if grep -q 'muslenvshow: PATH=' "${SERIAL_LOG}" 2>/dev/null; then
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

grep -q 'muslforkexecdriver:start' "${SERIAL_LOG}"
grep -q 'muslforkexecdriver:parent child=' "${SERIAL_LOG}"

if grep -q 'muslenvshow: PATH=' "${SERIAL_LOG}"; then
    echo "musl fork+exec+wait smoke reached child image"
elif grep -q '#PF(User): pdpe-not-present at 0xFFFFFFFF00000030' "${SERIAL_LOG}" &&
     grep -q 'RIP: 000000000040260B' "${SERIAL_LOG}" &&
     grep -q 'nr=0x00000000000000DA' "${SERIAL_LOG}"; then
    echo "musl fork+exec+wait smoke reproduced pre-exec child fault"
else
    echo "musl fork+exec+wait smoke did not reach a known checkpoint" >&2
    exit 1
fi

tail -n 200 "${SERIAL_LOG}"
