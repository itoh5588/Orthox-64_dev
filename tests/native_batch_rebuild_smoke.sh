#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-batch-rebuild-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-batch-rebuild-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_batch_rebuild_smoke.sh"
SRC_DIR="rootfs/src"
MAKEFILE_PATH="${SRC_DIR}/Makefile.batch_rebuild"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
AT_BACKUP="$(mktemp)"
VM_BACKUP="$(mktemp)"
FTRUNC_BACKUP="$(mktemp)"
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
    if [ -f "${AT_BACKUP}" ]; then
        if [ -s "${AT_BACKUP}" ]; then
            cp "${AT_BACKUP}" "${SRC_DIR}/at_test.c"
        else
            rm -f "${SRC_DIR}/at_test.c"
        fi
        rm -f "${AT_BACKUP}"
    fi
    if [ -f "${VM_BACKUP}" ]; then
        if [ -s "${VM_BACKUP}" ]; then
            cp "${VM_BACKUP}" "${SRC_DIR}/vmerrno_test.c"
        else
            rm -f "${SRC_DIR}/vmerrno_test.c"
        fi
        rm -f "${VM_BACKUP}"
    fi
    if [ -f "${FTRUNC_BACKUP}" ]; then
        if [ -s "${FTRUNC_BACKUP}" ]; then
            cp "${FTRUNC_BACKUP}" "${SRC_DIR}/ftruncsave_test.c"
        else
            rm -f "${SRC_DIR}/ftruncsave_test.c"
        fi
        rm -f "${FTRUNC_BACKUP}"
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

mkdir -p "${SRC_DIR}"

if [ -f "${SRC_DIR}/at_test.c" ]; then cp "${SRC_DIR}/at_test.c" "${AT_BACKUP}"; else : > "${AT_BACKUP}"; fi
if [ -f "${SRC_DIR}/vmerrno_test.c" ]; then cp "${SRC_DIR}/vmerrno_test.c" "${VM_BACKUP}"; else : > "${VM_BACKUP}"; fi
if [ -f "${SRC_DIR}/ftruncsave_test.c" ]; then cp "${SRC_DIR}/ftruncsave_test.c" "${FTRUNC_BACKUP}"; else : > "${FTRUNC_BACKUP}"; fi
if [ -f "${MAKEFILE_PATH}" ]; then cp "${MAKEFILE_PATH}" "${MAKEFILE_BACKUP}"; else : > "${MAKEFILE_BACKUP}"; fi

cp user/at_test.c "${SRC_DIR}/at_test.c"
cp user/vmerrno_test.c "${SRC_DIR}/vmerrno_test.c"
cp user/ftruncsave_test.c "${SRC_DIR}/ftruncsave_test.c"
cat > "${MAKEFILE_PATH}" <<'EOF'
all: at_test_rebuilt vmerrno_test_rebuilt ftruncsave_test_rebuilt

at_test_rebuilt: at_test.c
	cc at_test.c -o at_test_rebuilt

vmerrno_test_rebuilt: vmerrno_test.c
	cc vmerrno_test.c -o vmerrno_test_rebuilt

ftruncsave_test_rebuilt: ftruncsave_test.c
	cc ftruncsave_test.c -o ftruncsave_test_rebuilt
EOF

cat > "${SCRIPT_PATH}" <<'EOF'
set -x
export PATH=/bin:/usr/bin:/
echo native-batch-rebuild-start
cp /src/Makefile.batch_rebuild /work/Makefile
cp /src/at_test.c /work/at_test.c
cp /src/vmerrno_test.c /work/vmerrno_test.c
cp /src/ftruncsave_test.c /work/ftruncsave_test.c
cd /work
ls Makefile at_test.c vmerrno_test.c ftruncsave_test.c
make all
./at_test_rebuilt
./vmerrno_test_rebuilt
./ftruncsave_test_rebuilt
echo native-batch-rebuild: PASS
echo native-batch-rebuild-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_batch_rebuild_smoke.sh
EOF

make "${ISO}" >/tmp/native-batch-rebuild-build.out

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

echo "Waiting for native batch rebuild smoke..."
for _ in $(seq 1 180); do
    if grep -q 'native-batch-rebuild: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-batch-rebuild: PASS' "${SERIAL_LOG}"; then
    echo "native batch rebuild smoke FAIL: pass marker not found"
    tail -n 260 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-batch-rebuild-start' "${SERIAL_LOG}"
grep -q 'native-batch-rebuild-end' "${SERIAL_LOG}"
grep -q 'cc at_test.c -o at_test_rebuilt' "${SERIAL_LOG}"
grep -q 'cc vmerrno_test.c -o vmerrno_test_rebuilt' "${SERIAL_LOG}"
grep -q 'cc ftruncsave_test.c -o ftruncsave_test_rebuilt' "${SERIAL_LOG}"
grep -q 'at_test: PASS' "${SERIAL_LOG}"
grep -q 'vmerrno_test: PASS' "${SERIAL_LOG}"
grep -q 'ftruncsave_test: PASS' "${SERIAL_LOG}"

echo "native batch rebuild smoke PASS"
tail -n 300 "${SERIAL_LOG}"
