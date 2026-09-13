# Orthox-64 Kernel Source Structure

This directory contains the core source code for the Orthox-64 kernel.

## Current layout (2026-09-13)

Paths below are relative to `kernel/`. Build membership is defined in the
top-level [Makefile](../Makefile); a file in this directory is not necessarily
linked into every architecture.

- `kernel/`: shared subsystems and syscall implementations, plus dispatchers
  and wrappers with architecture-specific build membership.
- `x86_64/`: x86 hardware, entry code, and remaining x86 syscall helpers.
- `aarch64/`: AArch64 entry and hardware support (QEMU virt / Raspberry Pi 4).
- `riscv64/`: RISC-V entry and hardware support, including its own FS and
  socket backend.

### Shared syscall implementations

The September 8 duplication inventory is historical, not a current TODO list.
The following files are now linked by all three architectures:

| Owner | Main responsibilities |
|---|---|
| `sys_mmap.c` | `brk`, `mmap`, `munmap`, `mprotect`, `mremap` through architecture VM hooks |
| `sys_rlimit.c` | `getrlimit`, `setrlimit`, `prlimit64` |
| `sys_uname.c` | `uname`, using architecture identity hooks |
| `sys_task.c` | PID/UID/GID queries, futex, `set_tid_address`, `sysinfo`, `getcwd`, `nanosleep`, `clock_gettime`, `exit`, `wait4` |
| `sys_random.c` | `getrandom`, using `arch_random_fill` |
| `sys_signal.c` | Signal action/mask, pending signals, alternate stack |
| `sys_iov.c` | `readv` / `writev`, using the selected FS backend |
| `sys_access.c` | `faccessat` |
| `sys_lseek.c` | `lseek`, with an architecture FS size-refresh hook |
| `sys_tty.c` | `TIOCGPGRP` / `TIOCSPGRP` helpers and foreground process-group state |

### Remaining boundaries and follow-up

- x86 dispatches through `syscall.c`; AArch64/RISC-V use
  `linux_syscall.c` with their architecture entry code.
- x86/AArch64 build `sys_fs.c` and `fs.c`; RISC-V uses `riscv64/fs.c`.
  Sharing a syscall name does not imply a shared FS backend.
- `fstat` retains ABI conversion: x86_64 and asm-generic stat layouts differ.
  This is not an implementation duplicate to remove mechanically.
- ioctl foreground process-group handling is shared. Termios layouts
  (`orth_termios` versus `linux_termios`), `FIOCLEX` / `FIONCLEX` support,
  and console-fd validation still differ. The Linux dispatcher currently
  accepts termios/window-size requests without checking that the fd is a console.
- `exit` / `wait4` are shared, but runtime verification of bootstrap exit
  (`ppid == 0`) remains pending in the
  [September 13 log](../Docs/Daily_log/日報2026-09-13.md).

## File Breakdown

### Core and Initialization

- **`x86_64/init.c`**: Kernel entry point and early bring-up sequence. Receives control from Limine, initializes memory management, descriptor tables, interrupts, syscall entry, tasking, PCI, networking, USB, and starts the first user-space process.
- **`elf.c`**: ELF loader used by both initial task bring-up and `execve()` style process loading.
- **`x86_64/kassert.c`**: Kernel-wide `KASSERT()` / `KBUG_ON()` and `kernel_panic()` implementation. Halts the CPU and dumps the failed expression, function, file, and line on serial.

### Memory Management

- **`x86_64/pmm.c`**: Physical Memory Manager. Tracks free physical pages, provides page-granular allocation and free, and maintains per-page reference counts that back COW.
- **`x86_64/vmm.c`**: Virtual Memory Manager. Builds and updates x86_64 4-level page tables, maps user/kernel memory, switches address spaces, and handles page faults including the COW path.

### CPU Setup and Interrupts

- **`x86_64/gdt.c` / `x86_64/gdt_flush.S`**: GDT and TSS setup for kernel/user privilege transitions.
- **`x86_64/idt.c` / `x86_64/interrupt.S`**: IDT setup and low-level interrupt/exception entry stubs. Routes legacy PIC IRQs and MSI/MSI-X vectors through the central dispatcher.
- **`x86_64/lapic.c`**: Local APIC timer and timing support used by scheduling and `lwIP` timeouts.
- **`x86_64/pic.c`**: Legacy 8259 PIC masking/unmasking for IRQ routing compatibility.
- **`irq.c`**: Central legacy IRQ and MSI/MSI-X vector dispatcher. Drivers register handlers here at init time and the IDT layer calls into this module.

### Synchronization, Wait Primitives, and SMP

- **`x86_64/spinlock.c`**: Spinlock primitives, IRQ save/restore helpers, and the global kernel lock used to serialize coarse-grained kernel sections.
- **`wait.c`**: `wait_queue` and `completion` API. Provides `wait_event()`, `wait_event_timeout()`, `wake_up_one()`, `wake_up_all()`, and the `complete()` family used by interrupt-driven I/O.
- **`bottom_half.c`**: Deferred-work queue. IRQ handlers enqueue light callbacks and the idle path runs them, keeping interrupt context short.
- **`x86_64/smp.c`**: SMP bring-up. Starts APs, sets up per-CPU state, and provides inter-processor signaling helpers.

### Tasking and Scheduler

- **`task.c` / `x86_64/task_switch.S`**: Task object lifecycle, run queue primitives, task allocation and reap, and the low-level context switch routine.
- **`sched.c`**: Scheduler policy. Per-CPU run queue management, fork-spread CPU selection, timer tick, resched requests, `schedule()`, and the idle loop.
- **`task_exec.c`**: `execve()` implementation, ELF image replacement, initial user stack construction, and `argv`/`envp`/`auxv` placement.
- **`task_fork.c`**: Shared fork orchestration: address-space cloning through architecture hooks, fd/signal copying, and child syscall-frame setup.
- **`task_internal.h`**: Task-internal constants and helper declarations. Syscall frame types come through `include/arch_syscall.h`.

### Syscall Dispatch and Implementation

- **`syscall.c` / `x86_64/syscall_entry.S`**: x86 C dispatch lives in `syscall.c`, entry assembly in `x86_64/syscall_entry.S`, and MSR initialization in `x86_64/syscall_msr.c`.
- **`x86_64/sys_time.c`**: Remaining x86 time helpers live in `x86_64/sys_time.c`: RTC reads, `gettimeofday`, `sched_yield`, and `sleep_ms`. Shared clock/sleep logic lives in `sys_task.c`.
- **`sys_signal.c`**: Linux-compatible `rt_sigaction`, `rt_sigprocmask`, `sigpending`, `sigaltstack`, and the Orthox private signal wrappers.
- **`x86_64/sys_vm.c`**: Remaining x86 VM helpers live in `x86_64/sys_vm.c` (`sys_brk_init`, `madvise`). Main VM syscalls live in shared `sys_mmap.c`.
- **`sys_proc.c`**: x86-side `arch_prctl`, `set_robust_list`, `kill`, session and process-group handling. Exit/wait/PID/futex logic lives in `sys_task.c`; TTY foreground state lives in `sys_tty.c`.
- **`sys_fs.c`**: x86/AArch64 FS/fd wrappers, termios/ioctl, pselect, and mount helpers, mainly delegating to `fs.c`. Vector I/O lives in `sys_iov.c`, `faccessat` in `sys_access.c`, `lseek` in `sys_lseek.c`, and `getcwd` in `sys_task.c`. Dispatchers call `fs_readlinkat` directly.
- **`sys_net.c`**: Only the Orthox private DNS wrapper `sys_dns_lookup`. Dispatchers call `net_socket_*` directly for sockets.
- **`x86_64/sys_device.c`**: Orthox private device syscalls — framebuffer info and mapping, keyboard event read, sound (square wave / PCM), USB info / block read, CPU id / runq stats, fork-spread policy knob, and the low-level serial output helper.
- **`sys_random.c`**: Shared `getrandom`, delegating entropy generation to `arch_random_fill`.
- **`sys_trace.c`**: `ORTHOX_MEM_TRACE` and `ORTHOX_MEM_PROGRESS` helpers — `mmap`/`mremap`/`mprotect`/`munmap` memtrace formatting and `mmap`/`brk` progress counters.

### File System and Storage

- **`fs.c`**: VFS/fd implementation body. Owns the file descriptor table, mount handling, RAMFS, the path lookup and dispatch logic, and the `fs_*` API consumed by `sys_fs.c`.
- **`vfs.c`**: VFS helper for mount points and path normalization shared between FS modules.
- **`storage.c`**: Storage device abstraction. Registers backends (memory-backed and `virtio-blk`) and exposes `storage_read_blocks` / `storage_write_blocks` to the FS layer.
- **`xv6bio.c`**: xv6fs buffer cache. Provides `bget`/`brelse`/`bread`/`bwrite` on top of the storage abstraction.
- **`xv6log.c`**: xv6fs journaling layer. Wraps writes in transactions, commits to the on-disk log, and recovers after crash.
- **`xv6fs.c`**: xv6fs file system core (Orthox-64 extended). Inode allocation, bitmap, double/triple-indirect blocks, directory operations, and large-file write chunking.

### Console Input

- **`x86_64/keyboard.c`**: PS/2 keyboard driver feeding the shell's console input buffer.

### PCI, USB, and Sound

- **`x86_64/pci.c`**: PCI enumeration and discovery. Locates devices such as `virtio-blk`, `virtio-net`, audio, and xHCI, and provides MSI/MSI-X capability handling.
- **`usb.c`**: USB / xHCI host controller and mass-storage-oriented code used for USB storage access and rootfs mount experiments.
- **`x86_64/sound.c`**: AC97 / SB16 fallback PCM playback support.

### VirtIO and Networking

- **`x86_64/virtio.c`**: VirtIO common code — virtqueue layout helpers, descriptor ring setup, and shared MSI-X queue vector wiring.
- **`x86_64/virtio_blk.c`**: `virtio-blk` driver. Implements the block device backend for xv6fs with an inflight request pool, IRQ-driven completion, bottom-half used-ring reclaim, and timeout handling.
- **`x86_64/virtio_net.c`**: Minimal `virtio-net` driver — RX/TX virtqueues, MAC discovery, and IRQ-driven frame I/O with a polling fallback.
- **`net.c`**: Thin NIC abstraction layer. Exposes frame send, MAC lookup, RX callback registration, and polling to the upper networking code.
- **`lwip_port.c`**: `lwIP` integration layer (`NO_SYS=1`). Brings up the netif, runs DHCP/DNS/timeout processing, handles ARP/ICMP diagnostics, UDP echo, and kernel-side DNS lookup glue.
- **`net_socket.c`**: lwIP socket backend for x86/AArch64, called directly through `net_socket_*` by dispatchers. RISC-V uses `riscv64/net_socket.c`.

### Freestanding libc Fragments

- **`cstring.c`**: Minimal string and memory routines required by the kernel and vendored components such as `lwIP`.
- **`cstdio.c`**: Minimal formatted output helpers and serial-oriented stdio support used inside the kernel.
- **`cstdlib.c`**: Minimal libc-style utility functions needed by freestanding kernel code.
