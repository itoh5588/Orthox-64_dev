#!/bin/bash
# Put fw.elf into the boot BRAM contents of the bitstream (no re-synthesis).
# Writes ../build/mbv_soc_fw.bit; the FPGA then starts the firmware on its own
# after configuration, and fw/run.sh uses that bitstream when it exists.
set -e
here=$(cd "$(dirname "$0")" && pwd)
build=$here/../build
. /home/itoh/Xilinx-2025.2/2025.2/Vivado/settings64.sh
"$here/build.sh" >/dev/null
cd "$here/build"
updatemem -force -meminfo "$build/mbv_soc.mmi" -data "$here/build/fw.elf" \
    -proc soc_i/cpu -bit "$build/mbv_soc.bit" -out "$build/mbv_soc_fw.bit" \
    > "$here/build/updatemem.log"
grep -E 'Errors encountered|completed' "$here/build/updatemem.log"
