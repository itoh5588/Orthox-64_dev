# Orthox-64 (development repository)

This is the **development snapshot** of Orthox-64. The book's reference implementation lives in a separate, frozen repository.

**Orthox-64 is a hobby operating system that compiles its own kernel from within itself** — using a GCC toolchain ported to run natively on the OS — and boots a real userland: Python 3.12 with NumPy, BusyBox, a TCP/IP stack with HTTPS, and DOOM. **It runs on three ISAs: x86-64, aarch64 and RISC-V (riscv64).**

The running OS can rebuild its own kernel from source, and that kernel boots and runs — the **self-hosting loop** is closed.

| ISA | Self-hosting |
|---|---|
| **x86-64** | The OS builds its own kernel and boots it. |
| **aarch64** | The OS builds its own kernel and boots it — **on real Raspberry Pi 4 hardware** (23 Aug 2026). |
| **riscv64** | **The GCC running on the OS builds GCC itself** (3 Aug 2026). Its generated code matches the cross-built compiler except for a single `.ident` line. |

![Orthox-64 Desktop](assets/screenshot.png)

Japanese main README: [README.md](README.md)

## Highlights

- **Self-hosting:** Compiles and boots its own kernel entirely within the running OS, using a natively-ported GCC 4.7.4 / Binutils 2.26 toolchain (see the table above).
- **Dynamic userland:** Full dynamic linking via musl's dynamic linker — `.so` loading, `dlopen`/`dlsym`, TLS, C++ runtime support. Python 3.12 imports and runs NumPy 1.26.4.
- **Networking:** `virtio-net` + `lwIP` IPv4 stack (DHCP / DNS / ICMP / UDP / TCP / sockets), BusyBox `httpd`, and a BearSSL HTTPS client.
- **SMP:** 4-CPU bring-up in QEMU, LAPIC timer, per-CPU run queue, validated blocking-wakeup paths.
- **DOOM** (`doomgeneric`) runs.

## Quick Start

Reference host: **Ubuntu 24.04 (incl. WSL2)** or macOS. The host build uses `clang -target x86_64-elf` + `lld` — no separate cross GCC required.

```bash
# 1. Install build dependencies (Ubuntu 22.04 / 24.04 / WSL2)
sudo apt-get update
sudo apt-get install -y clang lld llvm build-essential make python3 \
  xorriso mtools qemu-system-x86 git

# 2. Build (produces orthos.iso)
make

# 3. Boot in QEMU (exit with Ctrl-A x)
make run
```

Full details, macOS instructions, and the on-OS GCC 4.7.4 toolchain build are in [INSTALL.md](INSTALL.md).

## Overview

- **Kernel:** 64-bit long mode, Limine boot, PMM/VMM paging, preemptive multitasking, SMP scheduler.
- **Filesystem:** VFS + read-write xv6fs (ported from xv6-riscv, triple-indirect blocks up to ~16 GB per file).
- **Ported:** musl 1.2.5 / BusyBox 1.27 / Binutils 2.26 / GCC 4.7.4 / Python 3.12.3 / NumPy 1.26.4 / doomgeneric.

## Raspberry Pi 4 (aarch64) Port

The kernel has been **ported to aarch64**. **On 15 August 2026 it booted on real Raspberry Pi 4 hardware for the first time**, and **on 23 August 2026 the self-hosting loop was closed on the real hardware.**

![First boot on real Raspberry Pi 4 hardware](assets/pi4-first-boot.png)

### Self-hosting on real hardware

Orthox running on a Raspberry Pi 4 **builds its own kernel from source and boots the result.**

- The in-OS GCC 4.7.4 and Binutils compile and link the kernel — 41 C files and 5 assembly files, 24,227 lines — in **about 50 minutes**.
- The resulting kernel boots: USB, the SD card, and the shell all come up.
- **Building again under that kernel produces a byte-for-byte identical image** (217,088 bytes) — a **stable fixed point**, verified.

After x86-64, **kernel self-hosting now holds on a second ISA** — and on real hardware, not in QEMU.

### Working on real hardware

- **Boot:** entered at EL2 via armstub8, dropped to EL1. Netboot over LAN is also supported.
- **MMU:** 4KB granule, 39-bit VA, kernel in TTBR1, running at high VA.
- **Storage:** SD card read/write over EMMC2, MBR then xv6fs.
- **Display:** text console and framebuffer on HDMI.
- **USB:** VL805 (xHCI) brought up over PCIe, keyboard input.
- **Audio:** PWM + DMA out to the 3.5 mm jack.
- **DOOM:** runs on the hardware, with sound effects and music.

See [`scripts/pi4/README.md`](scripts/pi4/README.md) for the procedure and the values measured on real hardware.

## License

Orthox-64 itself is released under the MIT License ([LICENSE](LICENSE)). The kernel includes filesystem code ported from xv6-riscv (MIT), and `ports/` bundles third-party components (musl, lwIP, BearSSL, Limine, CPython, zlib, BusyBox, GNU Make/Binutils/GCC). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Acknowledgements

- **[MikanOS](https://github.com/uchan-nos/mikanos)** by [uchan-nos](https://github.com/uchan-nos): kernel architecture reference.
- **[Limine](https://github.com/limine-bootloader/limine)**: bootloader.
- **[xv6-riscv](https://github.com/mit-pdos/xv6-riscv)** (MIT PDOS, MIT): source of the ported root filesystem (xv6fs).
