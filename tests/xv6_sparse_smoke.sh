#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
ROOTFS_IMG="${ROOTFS_IMG:-rootfs.img}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/xv6-sparse-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/xv6-sparse-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/xv6_sparse_smoke.sh"
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
        if [ -f "${ROOTFS_IMG}" ]; then
            python3 scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd "${BOOTCMD_BACKUP}" "${ROOTFS_IMG}" >/dev/null || true
        fi
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
echo xv6-sparse-smoke-start
/bin/xv6_sparse_test.elf
echo xv6-sparse-smoke-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/xv6_sparse_smoke.sh
EOF

make orthos.iso >/tmp/xv6-sparse-build.out

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

echo "Waiting for xv6 sparse smoke..."
for _ in $(seq 1 120); do
    if grep -q 'xv6-sparse-smoke: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'xv6-sparse-smoke: PASS' "${SERIAL_LOG}"; then
    echo "xv6 sparse smoke FAIL: pass marker not found"
    tail -n 120 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'xv6-sparse-smoke-start' "${SERIAL_LOG}"
grep -q 'xv6-sparse-smoke-end' "${SERIAL_LOG}"
grep -q 'xv6-sparse-smoke: PASS' "${SERIAL_LOG}"

echo "xv6 sparse smoke PASS"
tail -n 160 "${SERIAL_LOG}"
