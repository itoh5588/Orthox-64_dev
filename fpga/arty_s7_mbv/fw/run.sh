#!/bin/bash
# Load fw + Orthox kernel over JTAG and print the UART until a timeout.
# usage: run.sh [kernel.elf] [seconds] [rootfs.img] [input-file]
#   rootfs.img is loaded at INITRD_START (default 0x88000000) as the RAM disk.
#   input-file, if given, is sent to the UART line by line after boot.
set -e
here=$(cd "$(dirname "$0")" && pwd)
kernel=${1:-/home/itoh/Orthox-64/out/kernel-riscv64.elf}
secs=${2:-60}
rootfs=${3:-}
input=${4:-}
if [ -n "$rootfs" ]; then
    export INITRD_START=${INITRD_START:-0x88000000}
    export INITRD_END=$(printf "0x%x" $((INITRD_START + $(stat -c %s "$rootfs"))))
fi
gnu=${RISCV_GNU:-/home/itoh/Xilinx-2025.2/2025.2/gnu/riscv/lin/bin}
. /home/itoh/Xilinx-2025.2/2025.2/Vivado/settings64.sh
"$here/build.sh" >/dev/null
bss_start=$("$gnu/riscv64-unknown-elf-nm" "$kernel" | awk '$3=="__bss_start"{print "0x"$1}')
bss_end=$("$gnu/riscv64-unknown-elf-nm" "$kernel" | awk '$3=="__bss_end"{print "0x"$1}')
zs=$("$gnu/riscv64-unknown-elf-nm" "$here/build/fw.elf" | awk '$3=="fw_zero_start"{print "0x"$1}')
ze=$("$gnu/riscv64-unknown-elf-nm" "$here/build/fw.elf" | awk '$3=="fw_zero_end"{print "0x"$1}')
stty -F /dev/ttyUSB1 115200 raw -echo -ixon cs8 -cstopb -parenb
log="$here/build/uart.log"
: > "$log"
timeout "$secs" cat /dev/ttyUSB1 > "$log" &
cat_pid=$!
xsdb "$here/run.tcl" "${BIT:-$here/../build/mbv_soc.bit}" "$here/build/fw.elf" "$kernel" \
    "$bss_start" "$bss_end" "$zs" "$ze" "$rootfs" "${INITRD_START:-}" 2>&1 | grep -E '^(fw|kernel|bss|rootfs|running)|rror'
if [ -n "$input" ]; then
    sleep "${INPUT_DELAY:-20}"
    # One character at a time: the UART Lite RX FIFO is 16 bytes and Orthox
    # polls it from the 10 Hz timer until RX interrupts are wired up.
    while IFS= read -r line; do
        printf "%s\r" "$line" | while IFS= read -r -n1 -d '' ch; do
            printf "%s" "$ch" > /dev/ttyUSB1
            sleep "${INPUT_CHAR_GAP:-0.12}"
        done
        sleep "${INPUT_GAP:-3}"
    done < "$input"
fi
wait $cat_pid || true
tr -d '\r' < "$log"
