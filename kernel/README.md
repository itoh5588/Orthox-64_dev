# Orthox-64 Kernel Source Structure

This directory contains the core source code for the Orthox-64 kernel.

## File Breakdown

### Core and Initialization
- **`init.c`**: Kernel entry point and early bring-up sequence. Receives control from Limine, initializes memory management, descriptor tables, interrupts, syscall entry, tasking, PCI, networking, USB, and starts the first user-space process.
- **`elf.c`**: ELF loader used by both initial task bring-up and `execve()` style process loading.
- **`kassert.c`**: Kernel-wide `KASSERT()` / `KBUG_ON()` and `kernel_panic()` implementation. Halts the CPU and dumps the failed expression, function, file, and line on serial.

### Memory Management
- **`pmm.c`**: Physical Memory Manager. Tracks free physical pages, provides page-granular allocation and free, and maintains per-page reference counts that back COW.
- **`vmm.c`**: Virtual Memory Manager. Builds and updates x86_64 4-level page tables, maps user/kernel memory, switches address spaces, and handles page faults including the COW path.

### CPU Setup and Interrupts
- **`gdt.c` / `gdt_flush.S`**: GDT and TSS setup for kernel/user privilege transitions.
- **`idt.c` / `interrupt.S`**: IDT setup and low-level interrupt/exception entry stubs. Routes legacy PIC IRQs and MSI/MSI-X vectors through the central dispatcher.
- **`lapic.c`**: Local APIC timer and timing support used by scheduling and `lwIP` timeouts.
- **`pic.c`**: Legacy 8259 PIC masking/unmasking for IRQ routing compatibility.
- **`irq.c`**: Central legacy IRQ and MSI/MSI-X vector dispatcher. Drivers register handlers here at init time and the IDT layer calls into this module.

### Synchronization, Wait Primitives, and SMP
- **`spinlock.c`**: Spinlock primitives, IRQ save/restore helpers, and the global kernel lock used to serialize coarse-grained kernel sections.
- **`wait.c`**: `wait_queue` and `completion` API. Provides `wait_event()`, `wait_event_timeout()`, `wake_up_one()`, `wake_up_all()`, and the `complete()` family used by interrupt-driven I/O.
- **`bottom_half.c`**: Deferred-work queue. IRQ handlers enqueue light callbacks and the idle path runs them, keeping interrupt context short.
- **`smp.c`**: SMP bring-up. Starts APs, sets up per-CPU state, and provides inter-processor signaling helpers.

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
- **`sys_net.c`**: Socket syscall wrappers (`socket`, `connect`, `bind`, `listen`, `accept`, `send*`, `recv*`, `shutdown`, `setsockopt`, `getsockname`, `getpeername`) and the Orthox private `ORTH_SYS_DNS_LOOKUP` wrapper. Backend implementations live in `net_socket.c`.
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
- **`keyboard.c`**: PS/2 keyboard driver feeding the shell's console input buffer.

### PCI, USB, and Sound
- **`pci.c`**: PCI enumeration and discovery. Locates devices such as `virtio-blk`, `virtio-net`, audio, and xHCI, and provides MSI/MSI-X capability handling.
- **`usb.c`**: USB / xHCI host controller and mass-storage-oriented code used for USB storage access and rootfs mount experiments.
- **`sound.c`**: AC97 / SB16 fallback PCM playback support.

### VirtIO and Networking
- **`virtio.c`**: VirtIO common code — virtqueue layout helpers, descriptor ring setup, and shared MSI-X queue vector wiring.
- **`virtio_blk.c`**: `virtio-blk` driver. Implements the block device backend for xv6fs with an inflight request pool, IRQ-driven completion, bottom-half used-ring reclaim, and timeout handling.
- **`virtio_net.c`**: Minimal `virtio-net` driver — RX/TX virtqueues, MAC discovery, and IRQ-driven frame I/O with a polling fallback.
- **`net.c`**: Thin NIC abstraction layer. Exposes frame send, MAC lookup, RX callback registration, and polling to the upper networking code.
- **`lwip_port.c`**: `lwIP` integration layer (`NO_SYS=1`). Brings up the netif, runs DHCP/DNS/timeout processing, handles ARP/ICMP diagnostics, UDP echo, and kernel-side DNS lookup glue.
- **`net_socket.c`**: Kernel socket backend on top of `lwIP`. Implements the `AF_INET` socket path for UDP and TCP — `net_socket_*` entry points are called from `sys_net.c`.

### Freestanding libc Fragments
- **`cstring.c`**: Minimal string and memory routines required by the kernel and vendored components such as `lwIP`.
- **`cstdio.c`**: Minimal formatted output helpers and serial-oriented stdio support used inside the kernel.
- **`cstdlib.c`**: Minimal libc-style utility functions needed by freestanding kernel code.
