# Orthox-64 Installation Guide

## Requirements

To build and run Orthox-64, you need the following tools on the host:

- **Host OS:** Linux (Ubuntu 24.04 + WSL2 is the reference environment) or macOS
- **C/C++ Compiler:** `clang` with `-target x86_64-elf` (no separate x86_64-elf-gcc cross toolchain is needed)
- **Linker:** `lld`
- **Build Tools:** `make`, `python3` (**3.10 or later** — the rootfs build scripts use modern type-hint syntax; the macOS system `python3` 3.9 is not sufficient)
- **Image Tools:** `xorriso`, `mtools`
- **Emulator:** `qemu-system-x86_64`

### Ubuntu 22.04 / 24.04 (incl. WSL2)

```bash
sudo apt-get update
sudo apt-get install -y \
  clang lld llvm \
  build-essential \
  make python3 \
  xorriso mtools \
  qemu-system-x86 \
  git
```

### macOS

```bash
brew install llvm lld make python3 xorriso mtools qemu git
```

`brew`-installed `clang` and `python3` may need to be on `PATH` ahead of the system versions (macOS ships Python 3.9, which is too old for the rootfs build scripts). Adjust your shell init if necessary, e.g. `export PATH="/opt/homebrew/bin:$PATH"`.

## Build Instructions

1. **Clone the repository**
   ```bash
   git clone https://github.com/yourusername/Orthox-64.git
   cd Orthox-64
   ```

2. **Build the kernel, userland, and a bootable ISO**
   ```bash
   make
   ```
   This produces `orthos.iso` (Limine-bootable) along with `kernel.elf` and user-space binaries.

## Running the OS

```bash
make run
```

Internally this invokes `tests/run_qemu_stdio.sh`, which boots `orthos.iso` under `qemu-system-x86_64` with serial console multiplexed to stdio (`-serial mon:stdio`).

To exit QEMU at any time, press `Ctrl-A x`.

## Optional: Native (On-OS) Toolchain

`ports/build_gcc.sh` and `ports/build_binutils.sh` build the GCC 4.7.4 / Binutils 2.26 toolchain that runs **inside** Orthox-64 (used for the self-hosted kernel build demonstrated in Day 43 of the project log). These scripts target a macOS development host and are not required for the standard host-side cross build above.
