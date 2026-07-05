#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-multifile-work-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-multifile-work-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_multifile_smoke.sh"
MAKEFILE_PATH="rootfs/src/Makefile.multifile_smoke"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
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
if [ -f "${MAKEFILE_PATH}" ]; then
    cp "${MAKEFILE_PATH}" "${MAKEFILE_BACKUP}"
else
    : > "${MAKEFILE_BACKUP}"
fi

mkdir -p rootfs/src
cat > "${MAKEFILE_PATH}" <<'EOF'
all: test_multi

test_multi: test_multi_main.o test_multi_helper.o
	cc test_multi_main.o test_multi_helper.o -o test_multi

test_multi_main.o: test_multi_main.c test_multi_shared.h
	cc -c test_multi_main.c -o test_multi_main.o

test_multi_helper.o: test_multi_helper.c test_multi_shared.h
	cc -c test_multi_helper.c -o test_multi_helper.o
EOF

cat > "${SCRIPT_PATH}" <<'EOF'
set -x
export PATH=/bin:/usr/bin:/
echo native-multifile-make-smoke-start
cp /src/Makefile.multifile_smoke /work/Makefile
cp /test_multi_main.c /work/test_multi_main.c
cp /test_multi_helper.c /work/test_multi_helper.c
cp /test_multi_shared.h /work/test_multi_shared.h
cd /work
ls Makefile test_multi_main.c test_multi_helper.c test_multi_shared.h
make
ls test_multi_main.o test_multi_helper.o test_multi
if [ -f /work/test_multi ]; then
    /work/test_multi
    echo "native-multifile-make-smoke: PASS"
else
    echo "ERROR: /work/test_multi not found"
fi
echo native-multifile-make-smoke-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_multifile_smoke.sh
EOF

make "${ISO}" >/tmp/native-multifile-work-build.out

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

echo "Waiting for native multi-file make smoke..."
for _ in $(seq 1 180); do
    if grep -q 'native-multifile-make-smoke: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-multifile-make-smoke: PASS' "${SERIAL_LOG}"; then
    echo "native multi-file make smoke FAIL: pass marker not found"
    tail -n 160 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-multifile-make-smoke-start' "${SERIAL_LOG}"
grep -q 'native-multifile-make-smoke-end' "${SERIAL_LOG}"
grep -q 'Hello from Orthox-64 Multi File Build! 72' "${SERIAL_LOG}"
grep -q 'cc -c test_multi_main.c -o test_multi_main.o' "${SERIAL_LOG}"
grep -q 'cc -c test_multi_helper.c -o test_multi_helper.o' "${SERIAL_LOG}"
grep -q 'cc test_multi_main.o test_multi_helper.o -o test_multi' "${SERIAL_LOG}"

echo "native multi-file make smoke PASS"
tail -n 200 "${SERIAL_LOG}"
