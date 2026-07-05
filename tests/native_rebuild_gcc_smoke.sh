#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-rebuild-gcc-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-rebuild-gcc-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_rebuild_gcc_smoke.sh"
SRC_PATH="rootfs/src/gcc.c"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
SRC_BACKUP="$(mktemp)"
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
    if [ -f "${SRC_BACKUP}" ]; then
        if [ -s "${SRC_BACKUP}" ]; then
            cp "${SRC_BACKUP}" "${SRC_PATH}"
        else
            rm -f "${SRC_PATH}"
        fi
        rm -f "${SRC_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
else
    : > "${SCRIPT_BACKUP}"
fi
if [ -f "${SRC_PATH}" ]; then
    cp "${SRC_PATH}" "${SRC_BACKUP}"
else
    : > "${SRC_BACKUP}"
fi

mkdir -p rootfs/src
cp user/gcc.c "${SRC_PATH}"

cat > "${SCRIPT_PATH}" <<'EOF'
set -x
export PATH=/bin:/usr/bin:/
echo native-rebuild-gcc-start
cp /src/gcc.c /work/gcc.c
cp /test_native.c /work/test_native.c
cd /work
ls gcc.c test_native.c
cc gcc.c -o gcc_rebuilt
if [ ! -f /work/gcc_rebuilt ]; then
    echo "ERROR: /work/gcc_rebuilt not found"
    exit 1
fi
/work/gcc_rebuilt test_native.c -o test_native_rebuilt
if [ -f /work/test_native_rebuilt ]; then
    /work/test_native_rebuilt
    echo "native-rebuild-gcc: PASS"
else
    echo "ERROR: /work/test_native_rebuilt not found"
fi
echo native-rebuild-gcc-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_rebuild_gcc_smoke.sh
EOF

make "${ISO}" >/tmp/native-rebuild-gcc-build.out

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

echo "Waiting for native rebuild gcc smoke..."
for _ in $(seq 1 180); do
    if grep -q 'native-rebuild-gcc: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-rebuild-gcc: PASS' "${SERIAL_LOG}"; then
    echo "native rebuild gcc smoke FAIL: pass marker not found"
    tail -n 240 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-rebuild-gcc-start' "${SERIAL_LOG}"
grep -q 'native-rebuild-gcc-end' "${SERIAL_LOG}"
grep -q 'Hello from Orthox-64 Native C Compiler!' "${SERIAL_LOG}"

echo "native rebuild gcc smoke PASS"
tail -n 260 "${SERIAL_LOG}"
