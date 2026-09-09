#!/bin/bash
set -euo pipefail

# Orthox-64 Native Toolchain Smoke Test v3 (NO libgcc)

ISO_IMAGE="${1:-orthos.iso}"
LOG_FILE="native_smoke.log"
TIMEOUT=120

rm -f "$LOG_FILE"
touch "$LOG_FILE"

# Start QEMU in background with serial redirected to file AND input from pipe
QEMU_CMD="qemu-system-x86_64 -m 512M -drive format=raw,file=$ISO_IMAGE -display none -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04"

echo "Running Native Toolchain Smoke Test (v3) on $ISO_IMAGE..."

# Use a named pipe for QEMU stdin
PIPE=$(mktemp -u)
mkfifo "$PIPE"
trap 'rm -f "$PIPE"' EXIT

# Run QEMU with input from FIFO
$QEMU_CMD < "$PIPE" > "$LOG_FILE" 2>&1 &
QEMU_PID=$!

# Function to send command to QEMU shell
send_cmd() {
    echo "Sending: $1"
    echo "$1" > "$PIPE"
}

# 1. Wait for shell prompt
COUNT=0
PROMPT_FOUND=0
while [ $COUNT -lt 60 ]; do
    if grep -q "Orthox-64 shell" "$LOG_FILE"; then
        PROMPT_FOUND=1
        break
    fi
    sleep 1
    COUNT=$((COUNT + 1))
done

if [ $PROMPT_FOUND -eq 0 ]; then
    echo "FAIL: Shell prompt not found in 60s"
    cat "$LOG_FILE"
    kill $QEMU_PID || true
    exit 1
fi

echo "Shell prompt found. Compiling test_native.c..."

# 2. Compile test_native.c
# cc wrapper uses -B/usr/bin to find as, ld, cc1
send_cmd "/usr/bin/cc /test_native.c -o /test_native.elf"

# 3. Wait for compilation to finish (grep for next prompt)
# gcc 4.7.4 on QEMU takes some time
COUNT=0
COMPILE_DONE=0
while [ $COUNT -lt 60 ]; do
    # We look for a prompt AFTER the compilation command
    # A simple way is to count occurrences or look for a marker
    # For now, let's just wait and send another command (like ls) as a marker
    sleep 10
    send_cmd "ls -l /test_native.elf"
    sleep 2
    if grep -q "/test_native.elf" "$LOG_FILE"; then
        COMPILE_DONE=1
        break
    fi
    COUNT=$((COUNT + 10))
done

if [ $COMPILE_DONE -eq 0 ]; then
    echo "FAIL: Compilation failed or timed out"
    cat "$LOG_FILE"
    kill $QEMU_PID || true
    exit 1
fi

echo "Compilation successful. Running /test_native.elf..."

# 4. Run the compiled binary
send_cmd "/test_native.elf"

# 5. Check output
COUNT=0
PASS=0
while [ $COUNT -lt 10 ]; do
    if grep -q "Hello from Orthox-64 Native C Compiler!" "$LOG_FILE"; then
        PASS=1
        break
    fi
    sleep 1
    COUNT=$((COUNT + 1))
done

if [ $PASS -eq 1 ]; then
    echo "PASS: Native compilation and execution successful without libgcc!"
    kill $QEMU_PID || true
    exit 0
else
    echo "FAIL: Execution failed or output incorrect"
    cat "$LOG_FILE"
    kill $QEMU_PID || true
    exit 1
fi
