#!/bin/bash
set -euo pipefail

# Orthox-64 Native Toolchain Smoke Test v4 (NO libgcc)
# Using auto-boot script method

ISO="orthos.iso"
SERIAL_LOG="native-toolchain-serial.log"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_compile_smoke.sh"

cleanup() {
    # Restore original boot files if needed
    git checkout "${BOOTCMD_PATH}" 2>/dev/null || true
    rm -f "${SCRIPT_PATH}"
}
trap cleanup EXIT

# 1. Create the auto-run script
cat > "${SCRIPT_PATH}" <<'EOF'
set -x
export PATH=/bin:/usr/bin:/
echo native-toolchain-smoke-start
# Try compile
/usr/bin/cc /test_native.c -o /test_native.elf
# Run it
if [ -f /test_native.elf ]; then
    /test_native.elf
else
    echo "ERROR: /test_native.elf not found"
fi
echo native-toolchain-smoke-end
EOF

# 2. Update bootcmd to run the script
echo "/bin/ash /etc/native_compile_smoke.sh" > "${BOOTCMD_PATH}"

# 3. Rebuild ISO
make ROOTFS_REBUILD=1 "${ISO}" >/tmp/native-build.out

rm -f "${SERIAL_LOG}"

# 4. Run QEMU
echo "Running Native Toolchain Smoke Test (v4)..."
qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 1G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us &
QEMU_PID=$!

# 5. Wait for the result
TIMEOUT=180
PASS=0
for i in $(seq 1 $TIMEOUT); do
    if grep -q "Hello from Orthox-64 Native C Compiler!" "${SERIAL_LOG}" 2>/dev/null; then
        PASS=1
        break
    fi
    if (( i % 10 == 0 )); then
        echo "Still waiting ($i/$TIMEOUT)..."
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true

if [ $PASS -eq 1 ]; then
    echo "PASS: Native compilation and execution successful without libgcc!"
    tail -n 50 "${SERIAL_LOG}"
    exit 0
else
    echo "FAIL: Result marker not found in ${SERIAL_LOG}"
    tail -n 100 "${SERIAL_LOG}"
    exit 1
fi
