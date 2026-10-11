#!/bin/bash
# Boot the board as at power-on (configure the FPGA only; fw loads the kernel
# from SD), type commands into ash, and print the UART.
# usage: boot-run.sh <seconds> <command>...    (each command one line)
#   INPUT_DELAY: seconds before the first command (default 25)
#   BIT: bitstream (default ../build/mbv_soc_fw.bit)
set -e
here=$(cd "$(dirname "$0")" && pwd)
secs=$1
shift
. /home/itoh/Xilinx-2025.2/2025.2/Vivado/settings64.sh
mkdir -p "$here/build"
stty -F /dev/ttyUSB1 115200 raw -echo -ixon cs8 -cstopb -parenb
log="$here/build/uart.log"
: > "$log"
timeout "$secs" cat /dev/ttyUSB1 > "$log" &
cat_pid=$!
xsdb "$here/cfgonly.tcl" "${BIT:-$here/../build/mbv_soc_fw.bit}" 2>&1 | grep -E 'configured|rror'
sleep "${INPUT_DELAY:-25}"
for line in "$@"; do
    printf '%s\r' "$line" > /dev/ttyUSB1
    sleep 1
done
wait $cat_pid || true
tr -d '\r\000' < "$log"
