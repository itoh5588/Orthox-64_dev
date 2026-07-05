#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/ac97-smoke-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/ac97-smoke-qemu.out}"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

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

echo "Waiting for AC97 boot smoke..."
for _ in $(seq 1 45); do
    if grep -q 'pcm submitted=' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q '\[sound\] AC97 detected' "${SERIAL_LOG}" 2>/dev/null; then
    echo "AC97 was not detected"
    tail -n 160 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

if ! grep -q '\[ac97\] initialized' "${SERIAL_LOG}" 2>/dev/null; then
    echo "AC97 backend did not initialize"
    tail -n 160 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

if ! grep -q 'sound test start' "${SERIAL_LOG}" 2>/dev/null; then
    echo "testsound did not start"
    tail -n 160 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

if ! grep -q 'pcm test start' "${SERIAL_LOG}" 2>/dev/null; then
    echo "PCM phase did not start"
    tail -n 160 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

PCM_LINE="$(grep 'pcm submitted=' "${SERIAL_LOG}" | tail -n 1)"
PCM_VALUE="${PCM_LINE##*=}"
PCM_VALUE="${PCM_VALUE%%[!0-9-]*}"

if [ -z "${PCM_VALUE}" ] || [ "${PCM_VALUE}" -le 0 ]; then
    echo "PCM submit failed: ${PCM_LINE}"
    tail -n 160 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

echo "AC97 smoke PASS: ${PCM_LINE}"
tail -n 120 "${SERIAL_LOG}"
