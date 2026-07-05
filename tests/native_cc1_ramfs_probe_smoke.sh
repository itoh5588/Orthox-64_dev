#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-cc1-ramfs-probe-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-cc1-ramfs-probe-qemu.out}"
MODE="${CC1_RAMFS_PROBE_MODE:-ramfs}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_cc1_ramfs_probe_smoke.sh"
SRC_DIR="rootfs/src"
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

mkdir -p "${SRC_DIR}"
cp include/syscall.h "${SRC_DIR}/syscall.h"
cp user/sh.c "${SRC_DIR}/sh_probe_full.c"

printf "CC1_RAMFS_PROBE_MODE='%s'\n" "${MODE}" > "${SCRIPT_PATH}"
cat >> "${SCRIPT_PATH}" <<'EOF'
set -ex
export PATH=/bin:/usr/bin:/
echo native-cc1-ramfs-probe-start
cp /src/syscall.h /work/syscall.h
cp /src/sh_probe_full.c /work/sh_ramfs.c
ls /src/sh_probe_full.c /work/sh_ramfs.c /work/syscall.h

if [ "${CC1_RAMFS_PROBE_MODE}" = "retro" ] || [ "${CC1_RAMFS_PROBE_MODE}" = "both" ]; then
    echo native-cc1-ramfs-probe:retro:start
    cc -std=c99 -DORTHOS_SH_ENABLE_USB=0 -S /src/sh_probe_full.c -o /work/sh_retro.s
    echo native-cc1-ramfs-probe:retro:end
fi

if [ "${CC1_RAMFS_PROBE_MODE}" = "ramfs" ] || [ "${CC1_RAMFS_PROBE_MODE}" = "both" ]; then
    echo native-cc1-ramfs-probe:ramfs:start
    cc -std=c99 -DORTHOS_SH_ENABLE_USB=0 -S /work/sh_ramfs.c -o /work/sh_ramfs.s
    echo native-cc1-ramfs-probe:ramfs:end
fi

echo native-cc1-ramfs-probe: PASS
echo native-cc1-ramfs-probe-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_cc1_ramfs_probe_smoke.sh
EOF

make "${ISO}" >/tmp/native-cc1-ramfs-probe-build.out

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

echo "Waiting for native cc1 ramfs probe..."
started_at="$(date +%s)"
last_marker_count=0
while true; do
    marker_count="$(grep -c 'native-cc1-ramfs-probe:.*:\(start\|end\)' "${SERIAL_LOG}" 2>/dev/null || true)"
    if [ "${marker_count}" != "${last_marker_count}" ]; then
        last_marker_count="${marker_count}"
        elapsed="$(( $(date +%s) - started_at ))"
        if [ -f "${SERIAL_LOG}" ]; then
            tail -n 30 "${SERIAL_LOG}" | grep 'native-cc1-ramfs-probe:' || true
        fi
        echo "elapsed=${elapsed}s"
    fi
    if grep -q 'native-cc1-ramfs-probe: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

echo "native cc1 ramfs probe PASS"
tail -n 360 "${SERIAL_LOG}"
