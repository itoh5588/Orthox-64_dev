#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-stage2-userland-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-stage2-userland-qemu.out}"
TIMEOUT_SECONDS="${STAGE2_USERLAND_TIMEOUT:-360}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_stage2_userland_smoke.sh"
SRC_DIR="rootfs/src"
MAKEFILE_PATH="${SRC_DIR}/Makefile.stage2_userland"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
GCC_BACKUP="$(mktemp)"
FILE_BACKUP="$(mktemp)"
SH_BACKUP="$(mktemp)"
SYSCALL_BACKUP="$(mktemp)"
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
    if [ -f "${GCC_BACKUP}" ]; then
        if [ -s "${GCC_BACKUP}" ]; then
            cp "${GCC_BACKUP}" "${SRC_DIR}/gcc.c"
        else
            rm -f "${SRC_DIR}/gcc.c"
        fi
        rm -f "${GCC_BACKUP}"
    fi
    if [ -f "${FILE_BACKUP}" ]; then
        if [ -s "${FILE_BACKUP}" ]; then
            cp "${FILE_BACKUP}" "${SRC_DIR}/file.c"
        else
            rm -f "${SRC_DIR}/file.c"
        fi
        rm -f "${FILE_BACKUP}"
    fi
    if [ -f "${SH_BACKUP}" ]; then
        if [ -s "${SH_BACKUP}" ]; then
            cp "${SH_BACKUP}" "${SRC_DIR}/sh.c"
        else
            rm -f "${SRC_DIR}/sh.c"
        fi
        rm -f "${SH_BACKUP}"
    fi
    if [ -f "${SYSCALL_BACKUP}" ]; then
        if [ -s "${SYSCALL_BACKUP}" ]; then
            cp "${SYSCALL_BACKUP}" "${SRC_DIR}/syscall.h"
        else
            rm -f "${SRC_DIR}/syscall.h"
        fi
        rm -f "${SYSCALL_BACKUP}"
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

if [ -f "${SRC_DIR}/gcc.c" ]; then cp "${SRC_DIR}/gcc.c" "${GCC_BACKUP}"; else : > "${GCC_BACKUP}"; fi
if [ -f "${SRC_DIR}/file.c" ]; then cp "${SRC_DIR}/file.c" "${FILE_BACKUP}"; else : > "${FILE_BACKUP}"; fi
if [ -f "${SRC_DIR}/sh.c" ]; then cp "${SRC_DIR}/sh.c" "${SH_BACKUP}"; else : > "${SH_BACKUP}"; fi
if [ -f "${SRC_DIR}/syscall.h" ]; then cp "${SRC_DIR}/syscall.h" "${SYSCALL_BACKUP}"; else : > "${SYSCALL_BACKUP}"; fi
if [ -f "${MAKEFILE_PATH}" ]; then cp "${MAKEFILE_PATH}" "${MAKEFILE_BACKUP}"; else : > "${MAKEFILE_BACKUP}"; fi

cp user/gcc.c "${SRC_DIR}/gcc.c"
cp user/file.c "${SRC_DIR}/file.c"
cp user/sh.c "${SRC_DIR}/sh.c"
cp include/syscall.h "${SRC_DIR}/syscall.h"
cat > "${MAKEFILE_PATH}" <<'EOF'
all: gcc_stage2 file_stage2 gcc_stage3 sh_stage2 test_native_stage2

gcc_stage1: gcc.c
	cc gcc.c -o gcc_stage1

gcc_stage2: gcc.c gcc_stage1
	./gcc_stage1 gcc.c -o gcc_stage2

file_stage2: file.c gcc_stage2
	./gcc_stage2 file.c -o file_stage2

gcc_stage3: gcc.c gcc_stage2
	./gcc_stage2 gcc.c -o gcc_stage3

sh_stage2: sh.c gcc_stage2
	./gcc_stage2 -std=c99 -DORTHOS_SH_ENABLE_USB=0 sh.c -o sh_stage2

test_native_stage2: test_native.c gcc_stage2
	./gcc_stage2 test_native.c -o test_native_stage2
EOF

cat > "${SCRIPT_PATH}" <<'EOF'
set -ex
export PATH=/bin:/usr/bin:/
echo native-stage2-userland-start
cp /src/Makefile.stage2_userland /work/Makefile
cp /src/gcc.c /work/gcc.c
cp /src/file.c /work/file.c
cp /src/sh.c /work/sh.c
cp /src/syscall.h /work/syscall.h
cp /test_native.c /work/test_native.c
cd /work
ls Makefile gcc.c file.c sh.c syscall.h test_native.c
make all
ls gcc_stage1 gcc_stage2 file_stage2 gcc_stage3 sh_stage2 test_native_stage2
./file_stage2 /bin/gcc /hello.txt /bin
./test_native_stage2
./gcc_stage3 test_native.c -o test_native_stage3
./test_native_stage3
echo native-stage2-userland: PASS
echo native-stage2-userland-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_stage2_userland_smoke.sh
EOF

make "${ISO}" >/tmp/native-stage2-userland-build.out

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

echo "Waiting for native stage2 userland smoke..."
for _ in $(seq 1 "${TIMEOUT_SECONDS}"); do
    if grep -q 'native-stage2-userland: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-stage2-userland: PASS' "${SERIAL_LOG}"; then
    echo "native stage2 userland smoke FAIL: pass marker not found"
    tail -n 360 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-stage2-userland-start' "${SERIAL_LOG}"
grep -q 'native-stage2-userland-end' "${SERIAL_LOG}"
grep -q 'cc gcc.c -o gcc_stage1' "${SERIAL_LOG}"
grep -q './gcc_stage1 gcc.c -o gcc_stage2' "${SERIAL_LOG}"
grep -q './gcc_stage2 file.c -o file_stage2' "${SERIAL_LOG}"
grep -q './gcc_stage2 gcc.c -o gcc_stage3' "${SERIAL_LOG}"
grep -q './gcc_stage2 -std=c99 -DORTHOS_SH_ENABLE_USB=0 sh.c -o sh_stage2' "${SERIAL_LOG}"
grep -q './gcc_stage2 test_native.c -o test_native_stage2' "${SERIAL_LOG}"
grep -q '/bin/gcc: ELF 64-bit LSB executable' "${SERIAL_LOG}"
grep -q '/hello.txt: ASCII text' "${SERIAL_LOG}"
grep -q '/bin: directory' "${SERIAL_LOG}"
grep -q 'Hello from Orthox-64 Native C Compiler!' "${SERIAL_LOG}"

echo "native stage2 userland smoke PASS"
tail -n 360 "${SERIAL_LOG}"
