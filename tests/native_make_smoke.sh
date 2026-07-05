#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/native-make-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/native-make-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_make_smoke.sh"
MAKEFILE_PATH="rootfs/src/Makefile.make_smoke"
SRC_PATH="rootfs/src/test_native.c"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
MAKEFILE_BACKUP="$(mktemp)"
SRC_BACKUP="$(mktemp)"
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
    if [ -f "${SRC_BACKUP}" ]; then
        if [ -s "${SRC_BACKUP}" ]; then
            cp "${SRC_BACKUP}" "${SRC_PATH}"
        else
            rm -f "${SRC_PATH}"
        fi
        rm -f "${SRC_BACKUP}"
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
if [ -f "${SRC_PATH}" ]; then
    cp "${SRC_PATH}" "${SRC_BACKUP}"
else
    : > "${SRC_BACKUP}"
fi

mkdir -p rootfs/src
cp rootfs/test_native.c "${SRC_PATH}"

cat > "${MAKEFILE_PATH}" <<'EOF'
all: hello

hello: test_native.c
	cc test_native.c -o hello
EOF

cat > "${SCRIPT_PATH}" <<'EOF'
set -x
export PATH=/bin:/usr/bin:/
echo native-make-start
cp /src/Makefile.make_smoke /work/Makefile
cp /src/test_native.c /work/test_native.c
echo direct-shell-path-start
/bin/sh -c 'printenv PATH'
echo direct-shell-path-done
echo direct-abs-cc-start
/bin/sh -c '/bin/cc /src/test_native.c -o /work/hello_abs'
if [ -f /work/hello_abs ]; then
    /work/hello_abs
    echo direct-abs-cc: PASS
else
    echo direct-abs-cc: FAIL
fi
echo direct-path-cc-start
/bin/sh -c 'cc /src/test_native.c -o /work/hello_path'
if [ -f /work/hello_path ]; then
    /work/hello_path
    echo direct-path-cc: PASS
else
    echo direct-path-cc: FAIL
fi
echo direct-cwd-cc-start
cd /work
/bin/sh -c 'cc test_native.c -o hello_cwd'
if [ -f /work/hello_cwd ]; then
    /work/hello_cwd
    echo direct-cwd-cc: PASS
else
    echo direct-cwd-cc: FAIL
fi
cd /work
export ORTHOS_SH_DEBUG=1
make
if [ ! -f /work/hello ]; then
    echo "ERROR: /work/hello not found"
    exit 1
fi
./hello
echo "native-make: PASS"
echo native-make-end
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_make_smoke.sh
EOF

make "${ISO}" >/tmp/native-make-build.out

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

echo "Waiting for native make smoke..."
for _ in $(seq 1 180); do
    if grep -q 'native-make: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q 'native-make: PASS' "${SERIAL_LOG}"; then
    echo "native make smoke FAIL: pass marker not found"
    tail -n 240 "${SERIAL_LOG}"
    exit 1
fi

grep -q 'native-make-start' "${SERIAL_LOG}"
grep -q 'native-make-end' "${SERIAL_LOG}"
grep -q 'Hello from Orthox-64 Native C Compiler!' "${SERIAL_LOG}"

echo "native make smoke PASS"
tail -n 260 "${SERIAL_LOG}"
