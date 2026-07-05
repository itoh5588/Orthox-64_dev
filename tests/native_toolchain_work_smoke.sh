#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-toolchain-work-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-toolchain-work-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_compile_smoke.sh"
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
echo native-toolchain-work-smoke-start
cp /test_native.c /work/test_native.c
cd /work
ls
cc test_native.c -o test_native
if [ -f /work/test_native ]; then
    /work/test_native
    echo "native-toolchain-smoke: PASS"
else
    echo "ERROR: /work/test_native not found"
fi
echo native-toolchain-work-smoke-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_compile_smoke.sh
EOF

make "${ISO}" >/tmp/native-toolchain-work-build.out

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

echo "Waiting for native /work smoke..."
for _ in $(seq 1 180); do
    if grep -q 'native-toolchain-smoke: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-toolchain-smoke: PASS' "${SERIAL_LOG}"; then
    echo "native /work smoke FAIL: pass marker not found"
    tail -n 160 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-toolchain-work-smoke-start' "${SERIAL_LOG}"
grep -q 'native-toolchain-work-smoke-end' "${SERIAL_LOG}"
grep -q 'Hello from Orthox-64 Native C Compiler!' "${SERIAL_LOG}"

echo "native /work smoke PASS"
tail -n 200 "${SERIAL_LOG}"
