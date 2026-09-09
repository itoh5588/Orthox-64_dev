#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-busybox-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-busybox-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/musl_busybox_smoke.ash"
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
echo musl-busybox-start
echo shell=$0
echo pwd-before
/bin/pwd
echo env-count
/bin/env | /bin/wc -l
echo path-before
/bin/printenv PATH
echo ls-bin
/bin/ls /bin
echo cat-hello
/bin/cat /hello.txt
echo head-hello
/bin/head -n 1 /hello.txt
echo tail-hello
/bin/tail -n 1 /hello.txt
echo stat-hello
/bin/stat /hello.txt
echo touch-bbx
/bin/touch /bbx.txt
echo ls-root
/bin/ls /
echo rm-bbx
/bin/rm /bbx.txt
echo attest
/bin/at_test.elf
echo musl-busybox-ok
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/musl_busybox_smoke.ash
EOF

make orthos.iso >/tmp/musl-busybox-build.out

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

echo "Waiting for musl BusyBox smoke..."
for _ in $(seq 1 120); do
    if grep -q 'musl-busybox-ok' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'musl-busybox-ok' "${SERIAL_LOG}"; then
    echo "musl BusyBox smoke FAIL: 'musl-busybox-ok' not found in serial log"
    tail -n 100 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'musl-busybox-start' "${SERIAL_LOG}"
grep -q 'shell=/etc/musl_busybox_smoke.ash' "${SERIAL_LOG}"
grep -q 'cat-hello' "${SERIAL_LOG}"
grep -q 'Hello from xv6fs!' "${SERIAL_LOG}"
grep -q 'stat-hello' "${SERIAL_LOG}"
grep -q '/hello.txt' "${SERIAL_LOG}"
grep -q 'touch-bbx' "${SERIAL_LOG}"
grep -q 'bbx.txt' "${SERIAL_LOG}"
grep -q 'attest' "${SERIAL_LOG}"
grep -q 'at_test: PASS' "${SERIAL_LOG}"
grep -q 'musl-busybox-ok' "${SERIAL_LOG}"

echo "musl BusyBox smoke PASS"
tail -n 160 "${SERIAL_LOG}"
echo "musl BusyBox smoke PASS"
tail -n 160 "${SERIAL_LOG}"
