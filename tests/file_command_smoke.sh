#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/file-command-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/file-command-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/file_command_smoke.sh"
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
set -x
export PATH=/bin:/usr/bin:/
echo file-command-smoke-start
file /bin/gcc /bin/cc1 /bin/ash /hello.txt /bin
echo file-command-smoke: PASS
echo file-command-smoke-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/file_command_smoke.sh
EOF

make "${ISO}" >/tmp/file-command-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 1G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for file command smoke..."
for _ in $(seq 1 180); do
    if grep -q 'file-command-smoke: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'file-command-smoke: PASS' "${SERIAL_LOG}"; then
    echo "file command smoke FAIL: pass marker not found"
    tail -n 180 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'file-command-smoke-start' "${SERIAL_LOG}"
grep -q 'file-command-smoke-end' "${SERIAL_LOG}"
grep -q '/bin/gcc: ELF 64-bit LSB executable' "${SERIAL_LOG}"
grep -q '/bin/cc1: ELF 64-bit LSB executable' "${SERIAL_LOG}"
grep -q '/bin/ash: ELF 64-bit LSB executable' "${SERIAL_LOG}"
grep -q '/hello.txt: ASCII text' "${SERIAL_LOG}"
grep -q '/bin: directory' "${SERIAL_LOG}"

echo "file command smoke PASS"
tail -n 220 "${SERIAL_LOG}"
