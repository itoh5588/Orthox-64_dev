#!/bin/bash
set -euo pipefail

# Orthox-64 Native Toolchain Smoke Test (NO libgcc)

ISO_IMAGE="${1:-orthos.iso}"
TIMEOUT=120

QEMU_CMD="qemu-system-x86_64 -m 512M -drive format=raw,file=$ISO_IMAGE -display none -serial stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04"

echo "Running Native Toolchain Smoke Test on $ISO_IMAGE..."

# Use expect or similar to interact with the shell and run the compiler
# For now, let's use a simple approach with a timeout and grep for PASS message

expect <<EOF
set timeout $TIMEOUT
spawn $QEMU_CMD
expect "Orthox-64 shell"
send "/usr/bin/cc /test_native.c -o /test_native.elf\r"
expect "Orthox-64 shell"
send "/test_native.elf\r"
expect "Hello from Orthox-64 Native C Compiler!" {
    puts "PASS: Native compilation successful"
    exit 0
}
expect eof
puts "FAIL: Native compilation failed"
exit 1
EOF
