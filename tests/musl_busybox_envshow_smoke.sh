#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-busybox-envshow-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-busybox-envshow-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/musl_busybox_envshow.ash"
SABA_MARKER_PATH="rootfs/bin/.autostart_saba"
SABA_BIN_PATH="rootfs/bin/saba"
DRIVER_ELF="user/muslbusyboxdriver.elf"
SH_ELF_PATH="user/sh.elf"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
SABA_MARKER_BACKUP="$(mktemp)"
SABA_BIN_BACKUP="$(mktemp)"
SH_ELF_BACKUP="$(mktemp)"
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
    if [ -f "${SABA_MARKER_BACKUP}" ]; then
        if [ -s "${SABA_MARKER_BACKUP}" ]; then
            cp "${SABA_MARKER_BACKUP}" "${SABA_MARKER_PATH}"
        else
            rm -f "${SABA_MARKER_PATH}"
        fi
        rm -f "${SABA_MARKER_BACKUP}"
    fi
    if [ -f "${SABA_BIN_BACKUP}" ]; then
        if [ -s "${SABA_BIN_BACKUP}" ]; then
            cp "${SABA_BIN_BACKUP}" "${SABA_BIN_PATH}"
        else
            rm -f "${SABA_BIN_PATH}"
        fi
        rm -f "${SABA_BIN_BACKUP}"
    fi
    if [ -f "${SH_ELF_BACKUP}" ]; then
        if [ -s "${SH_ELF_BACKUP}" ]; then
            cp "${SH_ELF_BACKUP}" "${SH_ELF_PATH}"
        fi
        rm -f "${SH_ELF_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
else
    : > "${SCRIPT_BACKUP}"
fi
if [ -f "${SABA_MARKER_PATH}" ]; then
    cp "${SABA_MARKER_PATH}" "${SABA_MARKER_BACKUP}"
    rm -f "${SABA_MARKER_PATH}"
else
    : > "${SABA_MARKER_BACKUP}"
fi
if [ -f "${SABA_BIN_PATH}" ]; then
    cp "${SABA_BIN_PATH}" "${SABA_BIN_BACKUP}"
    rm -f "${SABA_BIN_PATH}"
else
    : > "${SABA_BIN_BACKUP}"
fi
cp "${SH_ELF_PATH}" "${SH_ELF_BACKUP}"

make "${DRIVER_ELF}" >/tmp/musl-busybox-envshow-driver-build.out
cp "${DRIVER_ELF}" "${SH_ELF_PATH}"

cat > "${SCRIPT_PATH}" <<'EOF'
echo envshow-start
/bin/muslenvshow.elf
echo envshow-done
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/musl_busybox_envshow.ash
EOF

make orthos.iso >/tmp/musl-busybox-envshow-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for musl BusyBox envshow smoke..."
for _ in $(seq 1 90); do
    if grep -q 'envshow-done' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

grep -q 'envshow-start' "${SERIAL_LOG}"
grep -q 'muslenvshow: PATH=' "${SERIAL_LOG}"
grep -q 'envshow-done' "${SERIAL_LOG}"

echo "musl BusyBox envshow smoke PASS"
tail -n 160 "${SERIAL_LOG}"
