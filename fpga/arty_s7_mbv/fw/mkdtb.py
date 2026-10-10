#!/usr/bin/env python3
"""Write the flattened device tree for the Arty S7-50 MicroBlaze V SoC.

dtc is not installed here, so this emits the FDT blob directly. The tree
lists only what Orthox reads: timebase, memory, and the console UART.
"""
import os
import struct
import sys

CPU_HZ = 50_000_000
DDR_BASE, DDR_SIZE = 0x80000000, 0x10000000
UART_BASE, UART_SIZE = 0x40600000, 0x10000
INTC_BASE, INTC_SIZE = 0x41200000, 0x10000
SD_BASE, SD_SIZE = 0x40800000, 0x10000
INTC_PHANDLE = 1


def u32(*v):
    return struct.pack(">%dI" % len(v), *v)


def reg64(base, size):
    return struct.pack(">QQ", base, size)


def s(text):
    return text.encode() + b"\0"


TREE = ("", [
    ("#address-cells", u32(2)),
    ("#size-cells", u32(2)),
    ("compatible", s("digilent,arty-s7-50-mbv")),
    ("model", s("Arty S7-50 MicroBlaze V")),
], [
    ("chosen", [("stdout-path", s("/soc/serial@40600000"))], []),  # initrd added by main
    ("cpus", [
        ("#address-cells", u32(1)),
        ("#size-cells", u32(0)),
        ("timebase-frequency", u32(CPU_HZ)),
    ], [
        ("cpu@0", [
            ("device_type", s("cpu")),
            ("reg", u32(0)),
            ("compatible", s("riscv")),
            ("riscv,isa", s("rv64imafdc")),
            ("mmu-type", s("riscv,sv39")),
            ("clock-frequency", u32(CPU_HZ)),
        ], []),
    ]),
    ("memory@80000000", [
        ("device_type", s("memory")),
        ("reg", reg64(DDR_BASE, DDR_SIZE)),
    ], []),
    ("soc", [
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
        ("compatible", s("simple-bus")),
        ("ranges", b""),
    ], [
        ("interrupt-controller@41200000", [
            ("compatible", s("xlnx,xps-intc-1.00.a")),
            ("reg", reg64(INTC_BASE, INTC_SIZE)),
            ("interrupt-controller", b""),
            ("#interrupt-cells", u32(2)),
            ("xlnx,num-intr-inputs", u32(2)),
            ("phandle", u32(INTC_PHANDLE)),
        ], []),
        ("sd@40800000", [                       # rtl/sd_axi.vhd, Pmod JD
            ("compatible", s("orthox,sd-spi-axi")),
            ("reg", reg64(SD_BASE, SD_SIZE)),
        ], []),
        ("serial@40600000", [
            ("compatible", s("xlnx,xps-uartlite-1.00.a")),
            ("reg", reg64(UART_BASE, UART_SIZE)),
            ("current-speed", u32(115200)),
            ("interrupt-parent", u32(INTC_PHANDLE)),
            ("interrupts", u32(0, 0)),          # INTC input 0 (build.tcl concat/In0)
        ], []),
    ]),
])


def build(tree):
    strings = bytearray()
    offsets = {}
    struct_blk = bytearray()

    def name_off(name):
        if name not in offsets:
            offsets[name] = len(strings)
            strings.extend(name.encode() + b"\0")
        return offsets[name]

    def pad4(buf):
        while len(buf) % 4:
            buf.append(0)

    def node(name, props, children):
        struct_blk.extend(u32(1))
        struct_blk.extend(name.encode() + b"\0")
        pad4(struct_blk)
        for pname, val in props:
            struct_blk.extend(u32(3, len(val), name_off(pname)))
            struct_blk.extend(val)
            pad4(struct_blk)
        for child in children:
            node(*child)
        struct_blk.extend(u32(2))

    node(*tree)
    struct_blk.extend(u32(9))

    hdr_size = 40
    rsvmap = bytes(16)
    off_rsv = hdr_size
    off_struct = off_rsv + len(rsvmap)
    off_strings = off_struct + len(struct_blk)
    total = off_strings + len(strings)
    header = u32(0xD00DFEED, total, off_struct, off_strings, off_rsv,
                 17, 16, 0, len(strings), len(struct_blk))
    return header + rsvmap + bytes(struct_blk) + bytes(strings)


if __name__ == "__main__":
    # usage: mkdtb.py out.dtb [initrd_start initrd_end]
    out = sys.argv[1] if len(sys.argv) > 1 else "board.dtb"
    # NO_WFI=1 adds orthox,no-wfi for a bitstream whose WFI never returns
    # (MicroBlaze V with Wakeup tied low; build.tcl now ties it high).
    # SD_INSTALL=1 makes Orthox copy the initrd onto the SD card (LBA 0 on)
    # before mounting it. This overwrites the card.
    if os.environ.get("SD_INSTALL") == "1":
        TREE[2][0][1].append(("orthox,sd-install-initrd", b""))
    if os.environ.get("NO_WFI") == "1":
        TREE[2][1][2][0][1].append(("orthox,no-wfi", b""))
    if len(sys.argv) == 4:
        start, end = int(sys.argv[2], 0), int(sys.argv[3], 0)
        TREE[2][0][1].append(("linux,initrd-start", struct.pack(">Q", start)))
        TREE[2][0][1].append(("linux,initrd-end", struct.pack(">Q", end)))
    with open(out, "wb") as f:
        f.write(build(TREE))
