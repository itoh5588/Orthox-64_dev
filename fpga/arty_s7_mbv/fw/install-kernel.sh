#!/bin/bash
# Write an Orthox kernel ELF to the SD card (LBA 0x200000) through the
# firmware, then let the firmware boot it from there; prints the UART.
# Afterwards the board boots that kernel with no JTAG (bitstream from flash).
# usage: install-kernel.sh [kernel.elf] [seconds]
set -e
here=$(cd "$(dirname "$0")" && pwd)
kernel=${1:-/home/itoh/Orthox-64/out/kernel-riscv64-ash.elf}
secs=${2:-60}
gnu=${RISCV_GNU:-/home/itoh/Xilinx-2025.2/2025.2/gnu/riscv/lin/bin}
. /home/itoh/Xilinx-2025.2/2025.2/Vivado/settings64.sh
"$here/build.sh" >/dev/null
size_sym=$("$gnu/riscv64-unknown-elf-nm" "$here/build/fw.elf" | awk '$3=="fw_install_size"{print "0x"$1}')
stty -F /dev/ttyUSB1 115200 raw -echo -ixon cs8 -cstopb -parenb
log="$here/build/uart.log"
: > "$log"
timeout "$secs" cat /dev/ttyUSB1 > "$log" &
cat_pid=$!
BIT=${BIT:-$here/../build/mbv_soc_fw.bit}
[ -f "$BIT" ] || BIT=$here/../build/mbv_soc.bit
xsdb "$here/install.tcl" "$BIT" "$here/build/fw.elf" "$kernel" 0x8F000000 "$size_sym" \
    2>&1 | grep -E '^(fw|staged|running)|rror'
wait $cat_pid || true
tr -d '\r' < "$log"
