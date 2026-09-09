#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-toolchain-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-toolchain-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/musl_toolchain_smoke.ash"
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
export PATH=/bin:/usr/bin:/
echo musl-toolchain-smoke-start
rm -f /tmp/hello_run /tmp/hello_gen.s /tmp/hello_gen.o
if /bin/gcc -S hello.c -o /tmp/hello_gen.s; then
    echo musl-toolchain-step-s-ok
else
    echo musl-toolchain-step-s-fail
fi
if /bin/gcc -c hello.c -o /tmp/hello_gen.o; then
    echo musl-toolchain-step-c-ok
else
    echo musl-toolchain-step-c-fail
fi
if /bin/gcc hello.c -o /tmp/hello_run; then
    echo musl-toolchain-step-link-ok
else
    echo musl-toolchain-step-link-fail
fi
if [ -f /tmp/hello_run ]; then
    echo musl-toolchain-output-present
    /tmp/hello_run
else
    echo musl-toolchain-output-missing
fi
echo musl-toolchain-smoke-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/musl_toolchain_smoke.ash
EOF

make orthos.iso >/tmp/musl-toolchain-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for musl toolchain smoke..."
for _ in $(seq 1 120); do
    if grep -q 'musl-toolchain-smoke-end' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

grep -q 'Welcome to Orthox-64 Shell!' "${SERIAL_LOG}"
grep -q -- '--- OrthOS GCC Pipeline Started ---' "${SERIAL_LOG}"
grep -q -- '\[gcc\] Executing /bin/cc1' "${SERIAL_LOG}"
grep -q -- '\[gcc\] Executing /bin/as' "${SERIAL_LOG}"
grep -q -- '\[gcc\] Executing /bin/ld' "${SERIAL_LOG}"
grep -q -- 'musl-toolchain-step-s-ok' "${SERIAL_LOG}"
grep -q -- 'musl-toolchain-step-c-ok' "${SERIAL_LOG}"
grep -q -- 'musl-toolchain-step-link-ok' "${SERIAL_LOG}"
grep -q -- 'musl-toolchain-output-present' "${SERIAL_LOG}"
grep -q -- 'Hello, world' "${SERIAL_LOG}"
grep -q -- 'musl-toolchain-smoke-end' "${SERIAL_LOG}"

echo "musl toolchain smoke PASS"
tail -n 200 "${SERIAL_LOG}"
