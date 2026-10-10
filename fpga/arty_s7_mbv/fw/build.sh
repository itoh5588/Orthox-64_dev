#!/bin/sh
# Build fw.elf (M-mode firmware + embedded DTB) into ./build.
set -e
here=$(cd "$(dirname "$0")" && pwd)
gnu=${RISCV_GNU:-/home/itoh/Xilinx-2025.2/2025.2/gnu/riscv/lin/bin}
cc=$gnu/riscv64-unknown-elf-gcc
mkdir -p "$here/build"
# INITRD_START/INITRD_END (optional) are passed to /chosen as linux,initrd-*.
python3 "$here/mkdtb.py" "$here/build/board.dtb" ${INITRD_START:+$INITRD_START $INITRD_END}
"$cc" -march=rv64imac_zicsr -mabi=lp64 -mcmodel=medany -O2 -Wall -Wextra \
    -ffreestanding -fno-builtin -nostdlib -nostartfiles \
    -DDTB_PATH="\"$here/build/board.dtb\"" \
    -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-T,"$here/fw.ld" \
    -o "$here/build/fw.elf" "$here/start.S" "$here/sbi.c"
"$gnu/riscv64-unknown-elf-objdump" -h "$here/build/fw.elf" | grep -E '^ +[0-9]'
