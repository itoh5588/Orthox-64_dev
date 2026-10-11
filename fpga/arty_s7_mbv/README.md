# Arty S7-50 MicroBlaze V SoC prototype

This is the first FPGA feasibility test for the Orthox-64 RISC-V port. It creates a 64-bit MicroBlaze V with Sv39, I/D caches, DDR3 AXI memory, 115200 baud UARTLite, AXI timer, interrupt controller, 64 KiB boot BRAM, and MDM V JTAG debug. The CPU clock target is 50 MHz. Orthox boots on it over JTAG with the minimal M-mode firmware in `fw/` (see below).

## Build

Use Vivado ML Standard 2025.2 with Spartan-7 device support:

```sh
cd fpga/arty_s7_mbv
/home/itoh/Xilinx-2025.2/2025.2/Vivado/bin/vivado \
  -mode batch -nojournal -nolog -source build.tcl > build_run.log 2>&1
```

`build.tcl` reads the Arty S7 DDR3 configuration from `/home/itoh/PDP-11/w11/rtl/bplib/artys7/mig_a.prj`. Set `MIG_SOURCE_PRJ` to another copy of that configuration if needed. The script changes its controller interface from NATIVE to AXI in the ignored `build/` directory. The source configuration supplies the board's DDR3L part, pin positions, and MIG timing; verify it against the actual Arty S7-50 revision before programming hardware. The original file belongs to the w11 project and is not redistributed here.

The design places boot BRAM at `0x00000000`, 256 MiB DDR3 at `0x80000000`, and UARTLite/timer/interrupt controller at the addresses printed by `build.tcl`. Synthesis and routed reports are written to `build/`. Check `route_utilization.rpt`, `route_timing.rpt`, `route_drc.rpt`, and `route_clocks.rpt` before using the design on hardware.

## Firmware and booting Orthox

`fw/` holds a minimal M-mode firmware for boot BRAM at `0x0`: an SBI with BASE, TIME (backed by the Sstc `stimecmp` CSR), the legacy console calls, and an Orthox-specific call that hands AXI INTC interrupts (M-mode external) to the kernel as SEIP. `mkdtb.py` writes the device tree (no `dtc` needed) and `build.sh` builds `fw/build/fw.elf`.

`fw/run.sh` programs the bitstream, loads the firmware and a kernel ELF over JTAG, optionally loads an xv6fs image as the RAM disk (`/chosen linux,initrd-*`), and prints the UART:

```sh
fw/run.sh ../../out/kernel-riscv64-ash.elf 300 ../../out/rootfs-riscv64-ram.img [input.txt]
```

`fw/embed.sh` puts the firmware into the boot BRAM contents of the bitstream with `updatemem` (using the `build/mbv_soc.mmi` that `build.tcl` writes) and produces `build/mbv_soc_fw.bit`. With it the firmware starts by itself after configuration; when no kernel has been loaded over JTAG it reads one from the SD card, and waits if there is none. `fw/run.sh` uses this bitstream when it exists (override with `BIT=`).

Booting with no JTAG (power on):

```sh
fw/install-kernel.sh ../../out/kernel-riscv64-ash.elf   # kernel ELF -> SD LBA 0x200000, then boots it
vivado -mode batch -nolog -nojournal -source flash.tcl -tclargs build/mbv_soc_fw.bit [backup.bin]
```

`install-kernel.sh` stages the ELF file in DDR and the firmware writes it to the card (one header sector with a magic, the size and a checksum, then the file). At boot the firmware reads it back (about 1.5 s for 726 KB) and lays out the `PT_LOAD` segments, zeroing `.bss`. `flash.tcl` writes the bitstream to the MT25QL128 configuration flash (SPIx1; the MODE jumper JP1 must be on), optionally reading the whole flash back to `backup.bin` first. The rootfs is the xv6fs on the same card (LBA 0, see `SD_INSTALL=1`).

A JTAG load of an 8 MiB image takes about 75 s. `xsdb` does not clear `.bss`, so the firmware clears the range the loader writes into `fw_zero_start`/`fw_zero_end`.

`build.tcl` sets `C_USE_SLEEP=1` and ties the CPU `Wakeup` inputs high. With `Wakeup` low, WFI never returns and the hart cannot be halted from the debugger. Setting `NO_WFI=1` when building the firmware adds `orthox,no-wfi` to the device tree, which makes Orthox avoid WFI on older bitstreams.


