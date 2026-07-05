#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-sh-stage2-probe-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-sh-stage2-probe-qemu.out}"
TIMEOUT_SECONDS="${SH_STAGE2_PROBE_TIMEOUT:-90}"
PROBES="${SH_STAGE2_PROBES:-sh_probe_core.c sh_probe_usb_boot.c sh_probe_fat_helpers.c sh_probe_fat_iter.c sh_probe_fat_resolve.c sh_probe_fat_shell.c sh_probe_usb_load.c sh_probe_fat_lba.c sh_probe_fat_iter2.c sh_probe_fat_find.c sh_probe_fat_read_entry.c sh_probe_fat_resolve2.c sh_probe_listing.c sh_probe_run_builtin.c sh_probe_run_command_child.c sh_probe_shell_exec.c sh_probe_run_once.c sh_probe_run_extended.c sh_probe_run_line.c sh_probe_try_bootcmd.c sh_probe_full.c}"
PROBE_CFLAGS="${SH_STAGE2_PROBE_CFLAGS:-}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_sh_stage2_probe_smoke.sh"
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
sed -n '1,586p' user/sh.c > "${SRC_DIR}/sh_probe_core.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_core.c"
sed -n '1,873p' user/sh.c > "${SRC_DIR}/sh_probe_usb_boot.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_usb_boot.c"
sed -n '1,873p' user/sh.c > "${SRC_DIR}/sh_probe_fat_helpers.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_helpers.c"
sed -n '1,873p' user/sh.c > "${SRC_DIR}/sh_probe_fat_iter.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_iter.c"
sed -n '1,873p' user/sh.c > "${SRC_DIR}/sh_probe_fat_resolve.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_resolve.c"
sed -n '1,907p' user/sh.c > "${SRC_DIR}/sh_probe_fat_shell.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_shell.c"
sed -n '1,642p' user/sh.c > "${SRC_DIR}/sh_probe_usb_load.c"
printf '\n#endif\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_usb_load.c"
sed -n '1,646p' user/sh.c > "${SRC_DIR}/sh_probe_fat_lba.c"
printf '\n#endif\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_lba.c"
sed -n '1,780p' user/sh.c > "${SRC_DIR}/sh_probe_fat_iter2.c"
printf '\n#endif\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_iter2.c"
sed -n '1,789p' user/sh.c > "${SRC_DIR}/sh_probe_fat_find.c"
printf '\n#endif\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_find.c"
sed -n '1,800p' user/sh.c > "${SRC_DIR}/sh_probe_fat_read_entry.c"
printf '\n#endif\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_read_entry.c"
sed -n '1,825p' user/sh.c > "${SRC_DIR}/sh_probe_fat_resolve2.c"
printf '\n#endif\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_fat_resolve2.c"
sed -n '1,915p' user/sh.c > "${SRC_DIR}/sh_probe_listing.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_listing.c"
sed -n '1,1013p' user/sh.c > "${SRC_DIR}/sh_probe_run_builtin.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_run_builtin.c"
sed -n '1,1029p' user/sh.c > "${SRC_DIR}/sh_probe_run_command_child.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_run_command_child.c"
sed -n '1,1076p' user/sh.c > "${SRC_DIR}/sh_probe_shell_exec.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_shell_exec.c"
sed -n '1,1147p' user/sh.c > "${SRC_DIR}/sh_probe_run_once.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_run_once.c"
sed -n '1,1207p' user/sh.c > "${SRC_DIR}/sh_probe_run_extended.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_run_extended.c"
sed -n '1,1471p' user/sh.c > "${SRC_DIR}/sh_probe_run_line.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_run_line.c"
sed -n '1,1517p' user/sh.c > "${SRC_DIR}/sh_probe_try_bootcmd.c"
printf '\nint main(void) { return 0; }\n' >> "${SRC_DIR}/sh_probe_try_bootcmd.c"
cp user/sh.c "${SRC_DIR}/sh_probe_full.c"

printf "SH_STAGE2_PROBES='%s'\n" "${PROBES}" > "${SCRIPT_PATH}"
printf "SH_STAGE2_PROBE_CFLAGS='%s'\n" "${PROBE_CFLAGS}" >> "${SCRIPT_PATH}"
cat >> "${SCRIPT_PATH}" <<'EOF'
set -ex
export PATH=/bin:/usr/bin:/
echo native-sh-stage2-probe-start
WORK_DIR="/tmp"
cp /src/syscall.h "${WORK_DIR}/syscall.h"
for src in ${SH_STAGE2_PROBES:-sh_probe_core.c sh_probe_usb_boot.c sh_probe_fat_helpers.c sh_probe_fat_iter.c sh_probe_fat_resolve.c sh_probe_fat_shell.c sh_probe_usb_load.c sh_probe_fat_lba.c sh_probe_fat_iter2.c sh_probe_fat_find.c sh_probe_fat_read_entry.c sh_probe_fat_resolve2.c sh_probe_listing.c sh_probe_run_builtin.c sh_probe_run_command_child.c sh_probe_shell_exec.c sh_probe_run_once.c sh_probe_run_extended.c sh_probe_run_line.c sh_probe_try_bootcmd.c sh_probe_full.c}; do
    cp "/src/${src}" "${WORK_DIR}/${src}"
done
cd "${WORK_DIR}"
for src in ${SH_STAGE2_PROBES:-sh_probe_core.c sh_probe_usb_boot.c sh_probe_fat_helpers.c sh_probe_fat_iter.c sh_probe_fat_resolve.c sh_probe_fat_shell.c sh_probe_usb_load.c sh_probe_fat_lba.c sh_probe_fat_iter2.c sh_probe_fat_find.c sh_probe_fat_read_entry.c sh_probe_fat_resolve2.c sh_probe_listing.c sh_probe_run_builtin.c sh_probe_run_command_child.c sh_probe_shell_exec.c sh_probe_run_once.c sh_probe_run_extended.c sh_probe_run_line.c sh_probe_try_bootcmd.c sh_probe_full.c}; do
    echo native-sh-stage2-probe:${src}:start
    cc -std=c99 ${SH_STAGE2_PROBE_CFLAGS} -S "${src}" -o "${src%.c}.s"
    echo native-sh-stage2-probe:${src}:end
done
echo native-sh-stage2-probe: PASS
echo native-sh-stage2-probe-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_sh_stage2_probe_smoke.sh
EOF

make "${ISO}" >/tmp/native-sh-stage2-probe-build.out

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

echo "Waiting for native sh stage2 probe smoke..."
started_at="$(date +%s)"
last_marker_count=0
for _ in $(seq 1 "${TIMEOUT_SECONDS}"); do
    marker_count="$(grep -c 'native-sh-stage2-probe:.*:\(start\|end\)' "${SERIAL_LOG}" 2>/dev/null || true)"
    if [ "${marker_count}" != "${last_marker_count}" ]; then
        last_marker_count="${marker_count}"
        elapsed="$(( $(date +%s) - started_at ))"
        if [ -f "${SERIAL_LOG}" ]; then
            tail -n 30 "${SERIAL_LOG}" | grep 'native-sh-stage2-probe:' || true
        fi
        echo "elapsed=${elapsed}s"
    fi
    if grep -q 'native-sh-stage2-probe: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-sh-stage2-probe: PASS' "${SERIAL_LOG}"; then
    echo "native sh stage2 probe smoke FAIL: pass marker not found"
    tail -n 360 "${SERIAL_LOG}"
    exit 1
fi

echo "native sh stage2 probe smoke PASS"
tail -n 360 "${SERIAL_LOG}"
