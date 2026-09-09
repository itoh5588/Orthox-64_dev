# Orthox-64 Kernel Source Structure

This directory contains the core source code for the Orthox-64 kernel.

## Placement means the layer (reorganized 2026-09-07)

    kernel/           **ISA-independent layer** + the not-yet-unified syscall layer
    kernel/x86_64/    x86_64 implementation. **Whatever lives here is x86-specific**
    kernel/riscv64/   riscv64 implementation
    kernel/aarch64/   aarch64 implementation (Raspberry Pi 4 hardware / QEMU virt)

**Until then kernel/ served as both "the common layer" and "where x86 is
implemented".** Which layer a file belonged to could not be told without reading
`SRCS` / `AARCH64_SHARED_C_SRCS` / `RISCV64_SHARED_C_SRCS` in the Makefile, and
**18 names had a second, different implementation elsewhere.** In the list below,
files moved into the x86 implementation are shown with an `x86_64/` prefix.

**`syscall.c` and `sys_*.c` stay in kernel/.** Only x86 builds them, but they are
not x86-specific: aarch64 / riscv64 dispatch the same syscalls from
`linux_syscall.c`.

**The duplication was measured on 2026-09-08.** 60 syscalls exist on both sides:

| | count | shape |
|---|---|---|
| same `sys_X` on both paths | 13 | see the warning below — the *name* is shared, not the implementation |
| x86 adds one forwarding hop | 8 | sockets; folded away on 2026-09-08 |
| `linux_syscall.c` answers inline | 10 | `getpid`, `getuid`, ... |
| **genuinely separate implementations** | **29** | `mmap`, `brk`, `getrandom`, `uname`, `sysinfo`, `fstat`, `futex`, `ioctl`, `write`, `writev`, `lseek`, ... |

**Careful: `sys_X` is an arch hook, not a shared implementation.** 30 of these
names have two definitions, and which one links depends on the architecture:

    x86      syscall.c        -> sys_fs.c        -> fs.c
    aarch64  linux_syscall.c  -> sys_fs.c        -> fs.c
    riscv64  linux_syscall.c  -> riscv64/fs.c    (its own 1,512-line body)

`RISCV64_SHARED_C_SRCS` contains neither `fs.c` nor `sys_fs.c`. Rewriting a
`sys_X` call site to `fs_X` therefore breaks the riscv64 build — `fs_X` does not
exist there. Unifying these means unifying `riscv64/fs.c` with `fs.c` first.

### Where the 29 separate implementations live

Sorted by the file holding the x86 side, because that already answers most of
the "is this really a duplicate?" question:

| x86 side | syscalls | verdict |
|---|---|---|
| `x86_64/sys_vm.c` | `brk`, `mmap`, `munmap`, `mprotect`, `mremap` | **not a duplicate** — walks x86 page tables directly (moved to `x86_64/` on 2026-09-07) |
| `x86_64/sys_time.c` | `clock_gettime`, `nanosleep`, `sysinfo`, `uname`, `getrlimit`, `prlimit64` | mixed: the clock pair reads the CMOS RTC (x86-only), but `uname` / `sysinfo` / the rlimit pair are ISA-free and only differ in **which constants they report** |
| `sys_fs.c` | `fstat`, `write`, `writev`, `readv`, `lseek`, `ioctl`, `pipe2`, `faccessat`, `readlinkat` | blocked on `riscv64/fs.c` (see the warning above). `fstat` is **not** a duplicate: `struct kstat` is the x86_64 ABI layout and `struct linux_stat` the asm-generic one, so the conversion is correct and required |
| `sys_proc.c` | `futex`, `wait4`, `exit_group`, `set_tid_address` | **genuine candidates** — ISA-free on both sides |
| `sys_signal.c` | `rt_sigaction`, `rt_sigprocmask` | **genuine candidates** |
| `sys_random.c` | `getrandom` | **genuine candidate**, but the two sides disagree on policy: `arch_random_bytes` returns -1 with no hardware source, `arch_random_fill` always fills. Unifying changes behaviour on machines without an entropy source |

So of the 29, only about **7 are ready to be unified today**; the rest are
either correct arch splits or blocked on the `riscv64/fs.c` question. Several of
the remainder (`uname`, `sysinfo`) need a decision about *which values are
right*, not a code change — see the 2026-09-07 log, §5.

## File Breakdown

### Core and Initialization
- **`x86_64/init.c`**: Kernel entry point and early bring-up sequence. Receives control from Limine, initializes memory management, descriptor tables, interrupts, syscall entry, tasking, PCI, networking, USB, and starts the first user-space process.
- **`elf.c`**: ELF loader used by both initial task bring-up and `execve()` style process loading.
- **`x86_64/kassert.c`**: Kernel-wide `KASSERT()` / `KBUG_ON()` and `kernel_panic()` implementation. Halts the CPU and dumps the failed expression, function, file, and line on serial.

### Memory Management
- **`x86_64/pmm.c`**: Physical Memory Manager. Tracks free physical pages, provides page-granular allocation and free, and maintains per-page reference counts that back COW.
- **`x86_64/vmm.c`**: Virtual Memory Manager. Builds and updates x86_64 4-level page tables, maps user/kernel memory, switches address spaces, and handles page faults including the COW path.

### CPU Setup and Interrupts
- **`gdt.c` / `gdt_flush.S`**: GDT and TSS setup for kernel/user privilege transitions.
- **`idt.c` / `interrupt.S`**: IDT setup and low-level interrupt/exception entry stubs. Routes legacy PIC IRQs and MSI/MSI-X vectors through the central dispatcher.
- **`x86_64/lapic.c`**: Local APIC timer and timing support used by scheduling and `lwIP` timeouts.
- **`x86_64/pic.c`**: Legacy 8259 PIC masking/unmasking for IRQ routing compatibility.
- **`irq.c`**: Central legacy IRQ and MSI/MSI-X vector dispatcher. Drivers register handlers here at init time and the IDT layer calls into this module.

### Synchronization, Wait Primitives, and SMP
- **`x86_64/spinlock.c`**: Spinlock primitives, IRQ save/restore helpers, and the global kernel lock used to serialize coarse-grained kernel sections.
- **`wait.c`**: `wait_queue` and `completion` API. Provides `wait_event()`, `wait_event_timeout()`, `wake_up_one()`, `wake_up_all()`, and the `complete()` family used by interrupt-driven I/O.
- **`bottom_half.c`**: Deferred-work queue. IRQ handlers enqueue light callbacks and the idle path runs them, keeping interrupt context short.
- **`x86_64/smp.c`**: SMP bring-up. Starts APs, sets up per-CPU state, and provides inter-processor signaling helpers.

### Tasking and Scheduler
- **`task.c` / `task_switch.S`**: Task object lifecycle, run queue primitives, task allocation and reap, and the low-level context switch routine.
- **`sched.c`**: Scheduler policy. Per-CPU run queue management, fork-spread CPU selection, timer tick, resched requests, `schedule()`, and the idle loop.
- **`task_exec.c`**: `execve()` implementation, ELF image replacement, initial user stack construction, and `argv`/`envp`/`auxv` placement.
- **`task_fork.c`**: `fork()` implementation, COW PML4 copy, fd clone, signal handler copy, and child syscall frame setup.
- **`task_internal.h`**: Header shared between `task.c` and the `task_*.c` siblings. Owns `struct syscall_frame` and the internal helper prototypes.

### Syscall Dispatch and Implementation
- **`syscall.c` / `syscall_entry.S`**: SYSCALL entry stub, MSR initialization, and the C-side dispatcher that routes Linux-compatible syscall numbers and Orthox private syscall numbers to per-category implementation files.
- **`sys_time.c`**: `clock_gettime`, `gettimeofday`, `nanosleep`, `sched_yield`, `uname`, `sysinfo`, `getrlimit`, `prlimit64`, and the kernel `sleep_ms` helper.
- **`sys_signal.c`**: Linux-compatible `rt_sigaction`, `rt_sigprocmask`, `sigpending`, `sigaltstack`, and the Orthox private signal wrappers.
- **`sys_vm.c`**: User address space and VM syscalls (`brk`, `mmap`, `munmap`, `mprotect`, `mremap`, `madvise`) and the `sys_brk_init()` boot helper.
- **`sys_proc.c`**: Process lifecycle syscalls (`wait4`, `exit`, `kill`, PID/PPID/UID/GID stubs), futex/thread helpers (`arch_prctl`, `futex`, `set_tid_address`, `set_robust_list`), session and process group, and TTY foreground process group helpers.
- **`sys_fs.c`**: FS/fd syscall wrappers — `open`, `read`, `write`, `close`, `fcntl`, `dup2`, `pipe`/`pipe2`, `stat`/`fstat`/`lstat`/`fstatat`, `access`/`faccessat`, `readlink`/`readlinkat`, `lseek`, `getdents`/`getdents64`, `chdir`/`fchdir`/`getcwd`, `truncate`/`ftruncate`, `utimensat`, `sync`, `unlink`/`unlinkat`, `rename`, `chmod`, `mkdir`/`mkdirat`, `rmdir`, `pread64`/`pwrite64`, `readv`/`writev`, `ioctl`, termios, mount module root, mount status, and the private `ls` syscall. Each entry delegates to the matching `fs_*` implementation in `fs.c`.
- **`sys_net.c`**: Only the Orthox private `ORTH_SYS_DNS_LOOKUP` wrapper (backed by lwIP) is left. The 11 socket wrappers were pure 1:1 forwarders to `net_socket.c` and were folded away on 2026-09-08 — all three architectures now call `net_socket_*` directly from their dispatcher.
- **`sys_device.c`**: Orthox private device syscalls — framebuffer info and mapping, keyboard event read, sound (square wave / PCM), USB info / block read, CPU id / runq stats, fork-spread policy knob, and the low-level serial output helper.
- **`sys_random.c`**: `getrandom` syscall and the RDRAND-with-fallback entropy helper.
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
- **`net_socket.c`**: Kernel socket backend on top of `lwIP`. Implements the `AF_INET` socket path for UDP and TCP — `net_socket_*` entry points are called from `sys_net.c`.

### Freestanding libc Fragments
- **`cstring.c`**: Minimal string and memory routines required by the kernel and vendored components such as `lwIP`.
- **`cstdio.c`**: Minimal formatted output helpers and serial-oriented stdio support used inside the kernel.
- **`cstdlib.c`**: Minimal libc-style utility functions needed by freestanding kernel code.
