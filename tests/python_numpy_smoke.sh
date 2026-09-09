#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/python-numpy-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/python-numpy-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/numpy_smoke.py"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
SCRIPT_HAD_FILE=0
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
    if [ "${SCRIPT_HAD_FILE}" = "1" ]; then
        cp "${SCRIPT_BACKUP}" "${SCRIPT_PATH}"
    else
        rm -f "${SCRIPT_PATH}"
    fi
    rm -f "${SCRIPT_BACKUP}"
}
trap cleanup EXIT

mkdir -p "$(dirname "${BOOTCMD_PATH}")" "$(dirname "${SCRIPT_PATH}")"
cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
    SCRIPT_HAD_FILE=1
fi

cat > "${SCRIPT_PATH}" <<'EOF'
import sys

def mark(name):
    print(name, flush=True)

mark("numpy-smoke-start")

try:
    import numpy as np
except Exception as e:
    cur = e
    depth = 0
    while cur is not None and depth < 5:
        print(f"[{depth}]", type(cur).__name__, ":", str(cur)[:300])
        cur = cur.__cause__ or cur.__context__
        depth += 1
    sys.exit(1)
mark("numpy-import-done")

# 基本演算
a = np.array([1.0, 2.0, 3.0, 4.0])
b = np.array([10.0, 20.0, 30.0, 40.0])
c = a + b
assert list(c) == [11.0, 22.0, 33.0, 44.0], f"add failed: {c}"
mark("numpy-array-add-done")

# 行列積
m = np.array([[1, 2], [3, 4]], dtype=np.float64)
n = np.array([[5, 6], [7, 8]], dtype=np.float64)
p = m @ n
assert p[0, 0] == 19.0 and p[1, 1] == 50.0, f"matmul failed: {p}"
mark("numpy-matmul-done")

# sum / mean
arr = np.arange(100, dtype=np.float64)
assert arr.sum() == 4950.0, f"sum failed: {arr.sum()}"
mark("numpy-sum-done")
assert arr.mean() == 49.5, f"mean failed: {arr.mean()}"
mark("numpy-mean-done")

print("numpy-version:", np.__version__)
print("numpy-smoke-pass")
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/python3.12 /etc/numpy_smoke.py
EOF

echo "Building ${ISO}..."
make "${ISO}" > /tmp/python-numpy-build.out 2>&1

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

QEMU_ARGS=(
    -M q35
    -cpu max
    -m 2G
    -cdrom "${ISO}"
    -boot d
    -display none
    -serial "file:${SERIAL_LOG}"
    -no-reboot
    -drive if=none,id=rootfs,file=rootfs.img,format=raw
    -device virtio-blk-pci,drive=rootfs
)

echo "Running QEMU..."
qemu-system-x86_64 "${QEMU_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 "${PYTHON_NUMPY_SMOKE_TIMEOUT:-600}"); do
    if grep -q "numpy-smoke-pass" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q "numpy-import-error\|Traceback (most recent call last):\|EXCEPTION OCCURRED\|#PF(User):" "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if grep -q "numpy-smoke-pass" "${SERIAL_LOG}" \
    && ! grep -q "EXCEPTION OCCURRED" "${SERIAL_LOG}" \
    && ! grep -q "#PF(User):" "${SERIAL_LOG}"; then
    VER=$(grep "numpy-version:" "${SERIAL_LOG}" | sed 's/.*numpy-version: //')
    echo "Test passed: NumPy ${VER} on Orthox-64."
    exit 0
fi

echo "Test failed. Serial log follows:"
cat "${SERIAL_LOG}"
exit 1
