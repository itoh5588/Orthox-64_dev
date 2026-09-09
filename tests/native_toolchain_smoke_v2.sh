#!/bin/bash
set -euo pipefail

# Orthox-64 Native Toolchain Smoke Test v2 (NO libgcc)

ISO_IMAGE="${1:-orthos.iso}"
TIMEOUT=120

QEMU_CMD="qemu-system-x86_64 -m 512M -drive format=raw,file=$ISO_IMAGE -display none -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04"

echo "Running Native Toolchain Smoke Test (v2) on $ISO_IMAGE..."

# Command sequence to run inside the shell
# 1. Wait for shell
# 2. Compile test_native.c
# 3. Run the compiled binary
# 4. Wait for the output message

LOG_FILE="native_smoke.log"
rm -f "$LOG_FILE"

# Start QEMU and feed commands to its stdin
(
  sleep 10 # Wait for kernel/shell to start
  echo "/usr/bin/cc /test_native.c -o /test_native.elf"
  sleep 30 # Give compiler some time (gcc is heavy)
  echo "/test_native.elf"
  sleep 5
  echo "exit" # If shell supports it, or just let it time out
) | $QEMU_CMD | tee "$LOG_FILE" &

PID=$!

# Wait for the expected output in the log
COUNT=0
while [ $COUNT -lt $TIMEOUT ]; do
  if grep -q "Hello from Orthox-64 Native C Compiler!" "$LOG_FILE"; then
    echo "PASS: Native compilation successful"
    kill $PID || true
    exit 0
  fi
  sleep 1
  COUNT=$((COUNT + 1))
done

echo "FAIL: Timeout reached or compilation failed"
kill $PID || true
exit 1
