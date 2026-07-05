#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-rebuild-ftruncsave-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-rebuild-ftruncsave-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_rebuild_ftruncsave_smoke.sh"
SRC_PATH="rootfs/src/ftruncsave_test.c"
MAKEFILE_PATH="rootfs/src/Makefile.rebuild_ftruncsave"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
SRC_BACKUP="$(mktemp)"
MAKEFILE_BACKUP="$(mktemp)"
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
    if [ -f "${MAKEFILE_BACKUP}" ]; then
        if [ -s "${MAKEFILE_BACKUP}" ]; then
            cp "${MAKEFILE_BACKUP}" "${MAKEFILE_PATH}"
        else
            rm -f "${MAKEFILE_PATH}"
        fi
        rm -f "${MAKEFILE_BACKUP}"
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
if [ -f "${MAKEFILE_PATH}" ]; then
    cp "${MAKEFILE_PATH}" "${MAKEFILE_BACKUP}"
else
    : > "${MAKEFILE_BACKUP}"
fi

mkdir -p rootfs/src
cp user/ftruncsave_test.c "${SRC_PATH}"
cat > "${MAKEFILE_PATH}" <<'EOF'
all: ftruncsave_test_rebuilt

ftruncsave_test_rebuilt: ftruncsave_test.c
	cc ftruncsave_test.c -o ftruncsave_test_rebuilt
EOF

cat > "${SCRIPT_PATH}" <<'EOF'
set -x
export PATH=/bin:/usr/bin:/
echo native-rebuild-ftruncsave-start
cp /src/Makefile.rebuild_ftruncsave /work/Makefile
cp /src/ftruncsave_test.c /work/ftruncsave_test.c
cd /work
ls Makefile ftruncsave_test.c
make
if [ -f /work/ftruncsave_test_rebuilt ]; then
    /work/ftruncsave_test_rebuilt
    echo "native-rebuild-ftruncsave: PASS"
else
    echo "ERROR: /work/ftruncsave_test_rebuilt not found"
fi
echo native-rebuild-ftruncsave-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_rebuild_ftruncsave_smoke.sh
EOF

make "${ISO}" >/tmp/native-rebuild-ftruncsave-build.out

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

echo "Waiting for native rebuild ftruncsave smoke..."
for _ in $(seq 1 180); do
    if grep -q 'native-rebuild-ftruncsave: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-rebuild-ftruncsave: PASS' "${SERIAL_LOG}"; then
    echo "native rebuild ftruncsave smoke FAIL: pass marker not found"
    tail -n 200 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-rebuild-ftruncsave-start' "${SERIAL_LOG}"
grep -q 'native-rebuild-ftruncsave-end' "${SERIAL_LOG}"
grep -q 'cc ftruncsave_test.c -o ftruncsave_test_rebuilt' "${SERIAL_LOG}"
grep -q 'ftruncsave_test: PASS' "${SERIAL_LOG}"

echo "native rebuild ftruncsave smoke PASS"
tail -n 220 "${SERIAL_LOG}"
