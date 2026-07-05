#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-sh-stage2-timing-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-sh-stage2-timing-qemu.out}"
TIMEOUT_SECONDS="${SH_STAGE2_TIMING_TIMEOUT:-360}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_sh_stage2_timing_smoke.sh"
SRC_DIR="rootfs/src"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
GCC_BACKUP="$(mktemp)"
SH_BACKUP="$(mktemp)"
SYSCALL_BACKUP="$(mktemp)"
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
if [ -f "${SRC_DIR}/sh.c" ]; then cp "${SRC_DIR}/sh.c" "${SH_BACKUP}"; else : > "${SH_BACKUP}"; fi
if [ -f "${SRC_DIR}/syscall.h" ]; then cp "${SRC_DIR}/syscall.h" "${SYSCALL_BACKUP}"; else : > "${SYSCALL_BACKUP}"; fi

cp user/gcc.c "${SRC_DIR}/gcc.c"
cp user/sh.c "${SRC_DIR}/sh.c"
cp include/syscall.h "${SRC_DIR}/syscall.h"

cat > "${SCRIPT_PATH}" <<'EOF'
set -ex
export PATH=/bin:/usr/bin:/
echo native-sh-stage2-timing-start
cp /src/gcc.c /work/gcc.c
cp /src/sh.c /work/sh.c
cp /src/syscall.h /work/syscall.h
cd /work
ls gcc.c sh.c syscall.h
echo native-sh-stage2-timing:gcc_stage1:start
cc gcc.c -o gcc_stage1
echo native-sh-stage2-timing:gcc_stage1:end
echo native-sh-stage2-timing:gcc_stage2:start
./gcc_stage1 gcc.c -o gcc_stage2
echo native-sh-stage2-timing:gcc_stage2:end
echo native-sh-stage2-timing:cc1:start
./gcc_stage2 -std=c99 -DORTHOS_SH_ENABLE_USB=0 -S sh.c -o sh_stage2.s
echo native-sh-stage2-timing:cc1:end
echo native-sh-stage2-timing:as:start
/bin/as sh_stage2.s -o sh_stage2.o
echo native-sh-stage2-timing:as:end
echo native-sh-stage2-timing:ld:start
./gcc_stage2 sh_stage2.o -o sh_stage2
echo native-sh-stage2-timing:ld:end
ls sh_stage2.s sh_stage2.o sh_stage2
echo native-sh-stage2-timing: PASS
echo native-sh-stage2-timing-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_sh_stage2_timing_smoke.sh
EOF

make "${ISO}" >/tmp/native-sh-stage2-timing-build.out

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

echo "Waiting for native sh stage2 timing smoke..."
started_at="$(date +%s)"
last_marker_count=0
for _ in $(seq 1 "${TIMEOUT_SECONDS}"); do
    marker_count="$(grep -c 'native-sh-stage2-timing:.*:\(start\|end\)' "${SERIAL_LOG}" 2>/dev/null || true)"
    if [ "${marker_count}" != "${last_marker_count}" ]; then
        last_marker_count="${marker_count}"
        elapsed="$(( $(date +%s) - started_at ))"
        if [ -f "${SERIAL_LOG}" ]; then
            tail -n 20 "${SERIAL_LOG}" | grep 'native-sh-stage2-timing:' || true
        fi
        echo "elapsed=${elapsed}s"
    fi
    if grep -q 'native-sh-stage2-timing: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-sh-stage2-timing: PASS' "${SERIAL_LOG}"; then
    echo "native sh stage2 timing smoke FAIL: pass marker not found"
    tail -n 360 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-sh-stage2-timing-start' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:gcc_stage1:start' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:gcc_stage1:end' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:gcc_stage2:start' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:gcc_stage2:end' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:cc1:start' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:cc1:end' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:as:start' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:as:end' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:ld:start' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing:ld:end' "${SERIAL_LOG}"
grep -q 'native-sh-stage2-timing-end' "${SERIAL_LOG}"

echo "native sh stage2 timing smoke PASS"
tail -n 360 "${SERIAL_LOG}"
