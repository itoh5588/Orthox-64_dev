# OS 判定
UNAME_S := $(shell uname -s)

# コンパイラ設定
CC = clang
LD = lld -flavor gnu
TARGET = x86_64-elf
XGCC = x86_64-elf-gcc
XAR = x86_64-elf-ar

BUILD_DIR = build
USER_BUILD_DIR = $(BUILD_DIR)/musl/user

# フラグ
KERNEL_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-lto -fno-PIE -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
	-mcmodel=kernel -O2 -Wall -Wextra -Iinclude -Iports/lwip/src/include -MMD -MP
KERNEL_CFLAGS += $(KERNEL_CFLAGS_EXTRA)

KERNEL_LDFLAGS = -nostdlib -static -T scripts/kernel.ld

# libc / sysroot 設定
LIBC_IMPL = musl
MUSL_SYSROOT = ports/musl-install
USER_SYSROOT = $(MUSL_SYSROOT)

USER_INCLUDEDIR = $(USER_SYSROOT)/include
USER_LIBDIR = $(USER_SYSROOT)/lib
LIBC = $(USER_LIBDIR)/libc.a

# ユーザープログラム用フラグ
USER_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-PIE -O2 \
	-Iinclude -I$(USER_INCLUDEDIR) -MMD -MP

USER_LDFLAGS = -m elf_x86_64 -nostdlib -static -Ttext 0x400000

# x86_64-elf-gcc が無い環境では host clang/gcc の libgcc を使う
LIBGCC = 

# 出力ファイル名
KERNEL_ELF = kernel.elf
PIPE_TEST_ELF = user/pipetest.elf
PIPE_STRESS_ELF = user/pipestress.elf
SMP_STRESS_ELF = user/smpstress.elf
SCHEDMIX_ELF = user/schedmix.elf
REAP_TEST_ELF = user/reaptest.elf
SH_ELF = user/sh.elf
AT_TEST_ELF = user/at_test.elf
WADSTDIO_TEST_ELF = user/wadstdio_test.elf
LOOP_ELF = user/loop.elf
VRAM_TEST_ELF = user/testvram.elf
TIME_TEST_ELF = user/testtime.elf
SHOWCPU_ELF = user/showcpu.elf
RUNQSTAT_ELF = user/runqstat.elf
TCPHELLO_ELF = user/tcphello.elf
FORKCPU_TEST_ELF = user/forkcputest.elf
FORKMODE_ELF = user/forkmode.elf
KEY_TEST_ELF = user/testkey.elf
SOUND_TEST_ELF = user/testsound.elf
SIGNAL_TEST_ELF = user/signaltest.elf
TTY_TEST_ELF = user/ttytest.elf
SIGMASK_TEST_ELF = user/sigmasktest.elf
SIGACTION_TEST_ELF = user/sigactiontest.elf
FCHDIR_TEST_ELF = user/fchdirtest.elf
TTYLINK_TEST_ELF = user/ttylinktest.elf
MKDIR_TEST_ELF = user/mkdirtest.elf
WADHEAD_TEST_ELF = user/wadheadtest.elf
TICKRATE_TEST_ELF = user/tickratecheck.elf
UDP_ECHO_TEST_ELF = user/udpecho.elf
UDP_NB_TEST_ELF = user/udpnb.elf
HTTPS_FETCH_ELF = user/httpsfetch.elf
STATERRNO_ELF = user/staterrno.elf
PYENC_CHECK_ELF = user/pyenccheck.elf
MUSL_DIRCHECK_ELF = user/musldircheck.elf
MUSL_FORKPROBE_ELF = user/muslforkprobe.elf
MUSL_EXECPROBE_ELF = user/muslexecprobe.elf
MUSL_ENVSHOW_ELF = user/muslenvshow.elf
VBLK_TEST_ELF = user/vblk_test.elf
VBLK_STRESS_ELF = user/vblkstress.elf
KILO_ELF = user/kilo.elf
FILE_ELF = user/file.elf
VMERRNO_TEST_ELF = user/vmerrno_test.elf
COWTEST_ELF = user/cowtest.elf
FTRUNCSAVE_TEST_ELF = user/ftruncsave_test.elf
PREADPWRITE_TEST_ELF = user/preadpwrite_test.elf
XV6_SPARSE_TEST_ELF = user/xv6_sparse_test.elf
XV6_RECLAIM_TEST_ELF = user/xv6_reclaim_test.elf
XV6_LARGEWRITE_TEST_ELF = user/xv6_largewrite_test.elf
HELLO_DYN_ELF = user/hello_dyn.elf
DYNLINK_LIB_A_SO = user/libdyn_a.so
DYNLINK_LIB_B_SO = user/libdyn_b.so
DYNLINK_PLUGIN_SO = user/libdyn_plugin.so
DYNLINK_CPP_SO = user/libdyn_cpp.so
DYNLINK_MULTI_TLS_ELF = user/dynlink_multi_tls.elf
DYNLINK_DLOPEN_ELF = user/dynlink_dlopen.elf
DYNLINK_MALLOC_ELF = user/dynlink_malloc.elf
BUSYBOX_ASH_DYN_ELF = user/busybox-ash-dyn.elf
GCC_DYN_ELF = user/gcc-dyn.elf
RUST_HELLO_STD_ELF = ports/rust/hello_std
GCC_MUSL_ELF = user/gcc.elf
CC1_MUSL_ELF = user/cc1.elf
AS_MUSL_ELF = user/as.elf
LD_MUSL_ELF = user/ld.elf
MAKE_MUSL_ELF = user/make.elf
DOOM_MUSL_ELF = user/doomgeneric.elf
BUSYBOX_ASH_MUSL_ELF = user/busybox-ash.elf
BUSYBOX_ASH_APPLETS = ash sh busybox cat chmod cp echo env false head httpd ls mkdir mv printenv printf pwd rm rmdir stat tail test touch true wc
ISO = orthos.iso
ROOTFS_IMG = rootfs.img
XV6FS_IMG  = rootfs-xv6.img
ROOTFS_FILES = $(shell find rootfs -type f 2>/dev/null)
ROOTFS_REBUILD ?= 1
ROOTFS_VBLK_ARGS = -drive if=none,id=rootfs,file=$(ROOTFS_IMG),format=raw -device virtio-blk-pci,drive=rootfs

# ソース
SRCS = kernel/init.c kernel/kassert.c kernel/pmm.c kernel/elf.c kernel/gdt.c kernel/gdt_flush.S \
       kernel/vmm.c kernel/idt.c kernel/interrupt.S kernel/lapic.c kernel/sound.c kernel/syscall.c kernel/sys_trace.c kernel/sys_time.c kernel/sys_signal.c kernel/sys_vm.c kernel/sys_proc.c kernel/sys_fs.c kernel/sys_device.c kernel/sys_random.c kernel/sys_net.c kernel/syscall_entry.S \
       kernel/task.c kernel/task_exec.c kernel/task_fork.c kernel/sched.c kernel/wait.c kernel/bottom_half.c kernel/irq.c kernel/task_switch.S kernel/fs.c kernel/vfs.c kernel/storage.c kernel/xv6bio.c kernel/xv6log.c kernel/xv6fs.c kernel/pic.c kernel/keyboard.c kernel/pci.c kernel/net.c kernel/net_socket.c kernel/virtio.c kernel/virtio_net.c kernel/virtio_blk.c kernel/lwip_port.c kernel/cstring.c kernel/cstdio.c kernel/cstdlib.c kernel/usb.c kernel/smp.c kernel/spinlock.c

LWIP_CORE_SRCS = \
	ports/lwip/src/core/def.c \
	ports/lwip/src/core/dns.c \
	ports/lwip/src/core/inet_chksum.c \
	ports/lwip/src/core/init.c \
	ports/lwip/src/core/ip.c \
	ports/lwip/src/core/mem.c \
	ports/lwip/src/core/memp.c \
	ports/lwip/src/core/netif.c \
	ports/lwip/src/core/pbuf.c \
	ports/lwip/src/core/raw.c \
	ports/lwip/src/core/stats.c \
	ports/lwip/src/core/sys.c \
	ports/lwip/src/core/tcp.c \
	ports/lwip/src/core/tcp_in.c \
	ports/lwip/src/core/tcp_out.c \
	ports/lwip/src/core/timeouts.c \
	ports/lwip/src/core/udp.c
LWIP_IPV4_SRCS = \
	ports/lwip/src/core/ipv4/dhcp.c \
	ports/lwip/src/core/ipv4/etharp.c \
	ports/lwip/src/core/ipv4/icmp.c \
	ports/lwip/src/core/ipv4/ip4.c \
	ports/lwip/src/core/ipv4/ip4_addr.c
LWIP_NETIF_SRCS = ports/lwip/src/netif/ethernet.c
LWIP_SRCS = $(LWIP_CORE_SRCS) $(LWIP_IPV4_SRCS) $(LWIP_NETIF_SRCS)
BEARSSL_SRCS = $(shell find ports/BearSSL/src -type f -name '*.c' ! -name '._*' 2>/dev/null)
BEARSSL_OBJS = $(patsubst ports/BearSSL/src/%.c, $(BUILD_DIR)/bearssl/%.o, $(BEARSSL_SRCS))
BEARSSL_A = $(BUILD_DIR)/bearssl/libbearssl.a

# オブジェクトファイル (build ディレクトリ以下に配置)
OBJS = $(patsubst kernel/%.c, $(BUILD_DIR)/kernel/%.o, $(filter %.c, $(SRCS))) \
       $(patsubst kernel/%.S, $(BUILD_DIR)/kernel/%.o, $(filter %.S, $(SRCS))) \
       $(patsubst ports/lwip/src/%.c, $(BUILD_DIR)/lwip/%.o, $(LWIP_SRCS))

DEPS = $(OBJS:.o=.d) \
       $(USER_BUILD_DIR)/crt0.d $(USER_BUILD_DIR)/syscalls.d $(USER_BUILD_DIR)/syscall_wrap.d \
       $(USER_BUILD_DIR)/user_test.d $(USER_BUILD_DIR)/exec_test.d $(USER_BUILD_DIR)/pipe_test.d $(USER_BUILD_DIR)/pipestress.d $(USER_BUILD_DIR)/smpstress.d $(USER_BUILD_DIR)/schedmix.d \
       $(USER_BUILD_DIR)/at_test.d \
       $(USER_BUILD_DIR)/sh.d $(USER_BUILD_DIR)/gcc.d $(USER_BUILD_DIR)/as.d $(USER_BUILD_DIR)/ld.d \
       $(USER_BUILD_DIR)/loop.d $(USER_BUILD_DIR)/cowtest.d $(USER_BUILD_DIR)/rotest.d \
       $(USER_BUILD_DIR)/testvram.d $(USER_BUILD_DIR)/testtime.d $(USER_BUILD_DIR)/testkey.d \
       $(USER_BUILD_DIR)/showcpu.d $(USER_BUILD_DIR)/runqstat.d $(USER_BUILD_DIR)/tcphello.d \
       $(USER_BUILD_DIR)/testsound.d $(USER_BUILD_DIR)/mmaptest.d $(USER_BUILD_DIR)/reaptest.d \
       $(USER_BUILD_DIR)/robusttest.d $(USER_BUILD_DIR)/signaltest.d $(USER_BUILD_DIR)/ttytest.d \
       $(USER_BUILD_DIR)/sigmasktest.d $(USER_BUILD_DIR)/sigactiontest.d \
       $(USER_BUILD_DIR)/fchdirtest.d $(USER_BUILD_DIR)/ttylinktest.d \
       $(USER_BUILD_DIR)/mkdirtest.d $(USER_BUILD_DIR)/wadheadtest.d \
       $(USER_BUILD_DIR)/vmerrno_test.d $(USER_BUILD_DIR)/ftruncsave_test.d \
       $(USER_BUILD_DIR)/wadstdio_test.d $(USER_BUILD_DIR)/udpecho.d $(USER_BUILD_DIR)/udpnb.d \
       $(USER_BUILD_DIR)/vblkstress.d

.PHONY: all clean run ac97run ac97smoke doom doomac97smoke musltoolchainsmoke muslforkprobesmoke muslexecprobesmoke muslforkexecwaitsmoke muslbusyboxsmoke muslbusyboxenvshowsmoke dynlinkrealappsmoke vmsyscallsmoke timesyscallsmoke signalsyscallsmoke ftruncsavesmoke preadpwritesmoke xv6sparsesmoke xv6reclaimsmoke xv6largewritesmoke virtionetirqsmoke virtioblkinflightsmoke virtioq35smoke irqbottomhalfstresssmoke irqbottomhalfsmpstresssmoke finalsmokesuite smprun smp4run netrun usb usb-img doommsulrun doommuslrun toolchain toolchain-musl user/doomgeneric.elf busybox-ash busybox-ash-musl busybox-ash-musl-install __busybox_ash_musl __busybox_ash_musl_install nativekernelbuildsmoke nativekernelbootsmoke pythonnumpysmoke

all: $(ISO)

include mk/user-musl.mk

user/crt0.o: user/crt0.S
	$(XGCC) -std=c11 -ffreestanding -fno-PIE -O2 -Iinclude -Iports/musl-install/include -c $< -o $@

user/syscalls.o: user/syscalls.c
	$(XGCC) -std=c11 -ffreestanding -fno-PIE -O2 -Iinclude -Iports/musl-install/include -c $< -o $@

toolchain: toolchain-musl

doom: $(DOOM_MUSL_ELF)

$(DOOM_MUSL_ELF): FORCE
	@if [ ! -d user/doomgeneric/doomgeneric ]; then \
		echo "ERROR: DOOM source is missing: user/doomgeneric/doomgeneric" >&2; \
		echo "Fetch doomgeneric upstream and apply/place the Orthox-64 port before running make doom." >&2; \
		echo "Example: git clone https://github.com/ozkl/doomgeneric /tmp/doomgeneric" >&2; \
		echo "Then place the ported source under user/doomgeneric/doomgeneric." >&2; \
		exit 1; \
	fi
	$(MAKE) -C user/doomgeneric/doomgeneric LIBC_IMPL=musl OUTPUT=doomgeneric.elf
	cp user/doomgeneric/doomgeneric/doomgeneric.elf $(DOOM_MUSL_ELF)

busybox-ash-musl:
	$(MAKE) -C $(CURDIR) LIBC_IMPL=musl __busybox_ash_musl

busybox-ash-musl-install:
	$(MAKE) -C $(CURDIR) LIBC_IMPL=musl __busybox_ash_musl_install

$(KERNEL_ELF): $(OBJS)
	$(LD) $(KERNEL_LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/kernel/%.o: kernel/%.S
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lwip/%.o: ports/lwip/src/%.c
	@mkdir -p $(@D)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(BUILD_DIR)/bearssl/%.o: ports/BearSSL/src/%.c
	@mkdir -p $(@D)
	$(CC) -target $(TARGET) -std=c99 -ffreestanding -fno-PIE -O2 -Iports/BearSSL/inc -Iports/BearSSL/src -I$(MUSL_SYSROOT)/include -MMD -MP -c $< -o $@

$(BEARSSL_A): $(BEARSSL_OBJS)
	@mkdir -p $(@D)
	ar rcs $@ $^

Limine/limine:
	@if [ -x template/limine/limine ]; then \
		cp template/limine/limine template/limine/limine-bios.sys template/limine/limine-bios-cd.bin template/limine/limine-uefi-cd.bin Limine/; \
		cp template/limine/BOOTX64.EFI template/limine/BOOTIA32.EFI Limine/; \
	elif [ -x OLD_limine/limine ]; then \
		cp OLD_limine/limine OLD_limine/limine-bios.sys OLD_limine/limine-bios-cd.bin OLD_limine/limine-uefi-cd.bin Limine/; \
		cp OLD_limine/BOOTX64.EFI OLD_limine/BOOTIA32.EFI Limine/; \
	elif [ -f Limine/configure ]; then \
		CC=cc CFLAGS="-O2 -pipe" LDFLAGS="" $(MAKE) -C Limine limine; \
	else \
		echo "ERROR: Limine build files are missing. Restore template/limine or run Limine/bootstrap + configure." >&2; \
		exit 1; \
	fi

TEST_ELFS = $(MMAP_TEST_ELF) $(REAP_TEST_ELF) $(ROBUST_TEST_ELF) $(VRAM_TEST_ELF) $(TIME_TEST_ELF) $(KEY_TEST_ELF) $(SOUND_TEST_ELF) $(SIGNAL_TEST_ELF) $(TTY_TEST_ELF) $(SIGMASK_TEST_ELF) $(SIGACTION_TEST_ELF) $(FCHDIR_TEST_ELF) $(TTYLINK_TEST_ELF) $(MKDIR_TEST_ELF) $(WADHEAD_TEST_ELF)

FORCE:

$(ROOTFS_IMG): FORCE busybox-ash-musl-install $(ROOTFS_FILES) $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls.o $(UDP_ECHO_TEST_ELF) $(UDP_NB_TEST_ELF) $(HTTPS_FETCH_ELF) $(TIME_TEST_ELF) $(TICKRATE_TEST_ELF) $(SHOWCPU_ELF) $(RUNQSTAT_ELF) $(TCPHELLO_ELF) $(FORKCPU_TEST_ELF) $(FORKMODE_ELF) $(PIPE_STRESS_ELF) $(SMP_STRESS_ELF) $(SCHEDMIX_ELF) $(REAP_TEST_ELF) $(SIGNAL_TEST_ELF) $(SIGMASK_TEST_ELF) $(SIGACTION_TEST_ELF) $(STATERRNO_ELF) $(PYENC_CHECK_ELF) $(MUSL_DIRCHECK_ELF) $(MUSL_FORKPROBE_ELF) $(MUSL_EXECPROBE_ELF) $(MUSL_ENVSHOW_ELF) $(VBLK_TEST_ELF) $(VBLK_STRESS_ELF) $(SOUND_TEST_ELF) $(GCC_MUSL_ELF) $(CC1_MUSL_ELF) $(AS_MUSL_ELF) $(LD_MUSL_ELF) $(MAKE_MUSL_ELF) $(KILO_ELF) $(FILE_ELF) $(VMERRNO_TEST_ELF) $(COWTEST_ELF) $(FTRUNCSAVE_TEST_ELF) $(PREADPWRITE_TEST_ELF) $(XV6_SPARSE_TEST_ELF) $(XV6_RECLAIM_TEST_ELF) $(XV6_LARGEWRITE_TEST_ELF) $(HELLO_DYN_ELF) $(DYNLINK_LIB_A_SO) $(DYNLINK_LIB_B_SO) $(DYNLINK_PLUGIN_SO) $(DYNLINK_CPP_SO) $(DYNLINK_MULTI_TLS_ELF) $(DYNLINK_DLOPEN_ELF) $(DYNLINK_MALLOC_ELF) $(BUSYBOX_ASH_DYN_ELF) $(GCC_DYN_ELF)
	@if [ "$(ROOTFS_REBUILD)" = "0" ] && [ -f "$(ROOTFS_IMG)" ]; then \
		echo "Keeping existing $(ROOTFS_IMG) (ROOTFS_REBUILD=0)"; \
	else \
			mkdir -p rootfs/bin rootfs/lib; \
		mkdir -p rootfs/work rootfs/src rootfs/tmp rootfs/home; \
		bash scripts/populate_c_env_musl.sh; \
		rm -f rootfs/bin/ash.orthos rootfs/bin/busybox.orthos; \
		rm -f rootfs/bin/cc1.orthos rootfs/bin/cc1.orthos.elf rootfs/bin/gcc.orthos.elf; \
		rm -f rootfs/bin/as.orthos rootfs/bin/ld.orthos; \
		rm -f rootfs/bin/gcc.elf rootfs/bin/cc1.elf rootfs/bin/as.elf rootfs/bin/ld.elf rootfs/bin/make.elf; \
		rm -f rootfs/crt0.orthos.o rootfs/syscalls.orthos.o rootfs/libc.orthos.a; \
		cp $(GCC_MUSL_ELF) rootfs/usr/bin/gcc; \
		cp $(GCC_MUSL_ELF) rootfs/usr/bin/cc; \
		cp $(CC1_MUSL_ELF) rootfs/usr/bin/cc1; \
		cp $(AS_MUSL_ELF) rootfs/usr/bin/as; \
		cp $(LD_MUSL_ELF) rootfs/usr/bin/ld; \
		cp $(MAKE_MUSL_ELF) rootfs/usr/bin/make; \
		cp $(SH_ELF) rootfs/usr/bin/sh; \
		cp $(GCC_MUSL_ELF) rootfs/bin/gcc; \
		cp $(GCC_MUSL_ELF) rootfs/bin/cc; \
		cp $(CC1_MUSL_ELF) rootfs/bin/cc1; \
		cp $(AS_MUSL_ELF) rootfs/bin/as; \
		cp $(LD_MUSL_ELF) rootfs/bin/ld; \
		cp $(MAKE_MUSL_ELF) rootfs/bin/make; \
		cp $(SH_ELF) rootfs/bin/sh; \
		chmod +x rootfs/usr/bin/gcc rootfs/usr/bin/cc rootfs/usr/bin/cc1 rootfs/usr/bin/as rootfs/usr/bin/ld rootfs/usr/bin/make; \
		chmod +x rootfs/usr/bin/sh; \
		chmod +x rootfs/bin/gcc rootfs/bin/cc rootfs/bin/cc1 rootfs/bin/as rootfs/bin/ld rootfs/bin/make; \
		chmod +x rootfs/bin/sh; \
		cp $(USER_BUILD_DIR)/crt0.o rootfs/crt0.o; \
		cp $(USER_BUILD_DIR)/syscalls.o rootfs/syscalls.o; \
		cp $(UDP_ECHO_TEST_ELF) rootfs/bin/udpecho.elf; \
		cp $(UDP_NB_TEST_ELF) rootfs/bin/udpnb.elf; \
		cp $(HTTPS_FETCH_ELF) rootfs/bin/httpsfetch.elf; \
		cp $(STATERRNO_ELF) rootfs/bin/staterrno.elf; \
		cp $(TIME_TEST_ELF) rootfs/bin/testtime.elf; \
		cp $(TICKRATE_TEST_ELF) rootfs/bin/tickratecheck.elf; \
		cp $(SHOWCPU_ELF) rootfs/bin/showcpu.elf; \
		cp $(RUNQSTAT_ELF) rootfs/bin/runqstat.elf; \
		cp $(TCPHELLO_ELF) rootfs/bin/tcphello.elf; \
		cp $(FORKCPU_TEST_ELF) rootfs/bin/forkcputest.elf; \
		cp $(FORKMODE_ELF) rootfs/bin/forkmode.elf; \
		cp $(PIPE_STRESS_ELF) rootfs/bin/pipestress.elf; \
		cp $(SMP_STRESS_ELF) rootfs/bin/smpstress.elf; \
		cp $(SCHEDMIX_ELF) rootfs/bin/schedmix.elf; \
		cp $(REAP_TEST_ELF) rootfs/bin/reaptest.elf; \
		cp $(SIGNAL_TEST_ELF) rootfs/bin/signaltest.elf; \
		cp $(SIGMASK_TEST_ELF) rootfs/bin/sigmasktest.elf; \
		cp $(SIGACTION_TEST_ELF) rootfs/bin/sigactiontest.elf; \
		cp $(PYENC_CHECK_ELF) rootfs/bin/pyenccheck; \
		cp $(MUSL_DIRCHECK_ELF) rootfs/bin/musldircheck; \
		cp $(MUSL_FORKPROBE_ELF) rootfs/bin/muslforkprobe.elf; \
		cp $(MUSL_EXECPROBE_ELF) rootfs/bin/muslexecprobe.elf; \
		cp $(MUSL_ENVSHOW_ELF) rootfs/bin/muslenvshow.elf; \
		cp $(VBLK_TEST_ELF) rootfs/bin/vblk_test; \
		cp $(VBLK_STRESS_ELF) rootfs/bin/vblkstress; \
		cp $(SOUND_TEST_ELF) rootfs/bin/testsound; \
		if [ -f "$(DOOM_MUSL_ELF)" ]; then cp $(DOOM_MUSL_ELF) rootfs/bin/doom-musl.elf; fi; \
		rm -f rootfs/bin/edit; \
		cp $(KILO_ELF) rootfs/bin/kilo; \
		cp $(FILE_ELF) rootfs/bin/file; \
		cp $(VMERRNO_TEST_ELF) rootfs/bin/vmerrno_test.elf; \
		cp $(COWTEST_ELF) rootfs/bin/cowtest.elf; \
		cp $(FTRUNCSAVE_TEST_ELF) rootfs/bin/ftruncsave_test.elf; \
		cp $(PREADPWRITE_TEST_ELF) rootfs/bin/preadpwrite_test.elf; \
		cp $(XV6_SPARSE_TEST_ELF) rootfs/bin/xv6_sparse_test.elf; \
		cp $(XV6_RECLAIM_TEST_ELF) rootfs/bin/xv6_reclaim_test.elf; \
		cp $(XV6_LARGEWRITE_TEST_ELF) rootfs/bin/xv6_largewrite_test.elf; \
		cp $(HELLO_DYN_ELF) rootfs/bin/hello_dyn.elf; \
			cp $(DYNLINK_MULTI_TLS_ELF) rootfs/bin/dynlink_multi_tls.elf; \
			cp $(DYNLINK_DLOPEN_ELF) rootfs/bin/dynlink_dlopen.elf; \
			cp $(DYNLINK_MALLOC_ELF) rootfs/bin/dynlink_malloc.elf; \
			cp $(BUSYBOX_ASH_DYN_ELF) rootfs/bin/busybox.dyn; \
			cp $(GCC_DYN_ELF) rootfs/bin/gcc.dyn; \
			cp $(DYNLINK_LIB_A_SO) rootfs/lib/libdyn_a.so; \
			cp $(DYNLINK_LIB_B_SO) rootfs/lib/libdyn_b.so; \
			cp $(DYNLINK_PLUGIN_SO) rootfs/lib/libdyn_plugin.so; \
			cp $(DYNLINK_CPP_SO) rootfs/lib/libdyn_cpp.so; \
			mkdir -p rootfs/kbuild/kernel rootfs/kbuild/lwip/core/ipv4 rootfs/kbuild/lwip/netif; \
			KBUILD=rootfs/src/kernel-build; \
			rm -rf "$$KBUILD"; \
			mkdir -p "$$KBUILD/ports/lwip/src"; \
			cp -r kernel "$$KBUILD/"; \
			cp -r include "$$KBUILD/"; \
			cp -r ports/lwip/src/include "$$KBUILD/ports/lwip/src/"; \
			cp -r ports/lwip/src/core "$$KBUILD/ports/lwip/src/"; \
			mkdir -p "$$KBUILD/ports/lwip/src/netif"; \
			cp ports/lwip/src/netif/ethernet.c "$$KBUILD/ports/lwip/src/netif/"; \
			mkdir -p "$$KBUILD/scripts"; \
			cp scripts/kernel.ld "$$KBUILD/scripts/"; \
			cp scripts/Makefile.kernel-native "$$KBUILD/Makefile"; \
			python3 scripts/build_rootfs_xv6fs.py rootfs $(ROOTFS_IMG); \
		fi

$(XV6FS_IMG): $(ROOTFS_FILES)
	python3 scripts/build_rootfs_xv6fs.py rootfs $(XV6FS_IMG)

rootfs-xv6.img: $(XV6FS_IMG)

ifeq ($(UNAME_S),Darwin)
LIMINE_BINARY_DEP =
else
LIMINE_BINARY_DEP = Limine/limine
endif

$(ISO): $(KERNEL_ELF) $(SH_ELF) iso/limine.conf $(LIMINE_BINARY_DEP) $(ROOTFS_IMG)
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	cp $(KERNEL_ELF) iso_root/boot/kernel.elf
	cp $(SH_ELF) iso_root/boot/sh.elf
	cp $(ROOTFS_IMG) iso_root/boot/rootfs.img
	cp iso/limine.conf iso_root/boot/limine/limine.conf

	mkdir -p iso_root/EFI/BOOT
	cp Limine/limine-bios.sys Limine/limine-bios-cd.bin Limine/limine-uefi-cd.bin iso_root/boot/limine/
	cp Limine/BOOTX64.EFI iso_root/EFI/BOOT/
	cp Limine/BOOTIA32.EFI iso_root/EFI/BOOT/
	xorriso -as mkisofs -v -R -r -J -b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
		-apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO)
ifneq ($(UNAME_S),Darwin)
	$(CURDIR)/Limine/limine bios-install $(ISO)
endif
	rm -rf iso_root


run: $(ISO)
	bash tests/run_qemu_stdio.sh

persist-run: ROOTFS_REBUILD=0
persist-run: $(ISO) $(ROOTFS_IMG)
	bash tests/run_qemu_stdio.sh \
		$(ROOTFS_VBLK_ARGS)

ac97run: $(ISO)
	bash tests/run_qemu_ac97.sh

ac97smoke: $(ISO)
	bash ./tests/ac97_smoke.sh $(ISO)

doomac97smoke: $(ISO)
	bash ./tests/doom_ac97_smoke.sh $(ISO)

musltoolchainsmoke: $(ISO)
	bash ./tests/musl_toolchain_smoke.sh

muslforkprobesmoke: $(ISO)
	bash ./tests/musl_forkprobe_smoke.sh $(ISO)

muslexecprobesmoke: $(ISO)
	bash ./tests/musl_execprobe_smoke.sh $(ISO)

muslforkexecwaitsmoke: $(ISO)
	bash ./tests/musl_forkexecwait_smoke.sh $(ISO)

muslbusyboxsmoke: $(ISO)
	bash ./tests/musl_busybox_smoke.sh $(ISO)

muslbusyboxenvshowsmoke: $(ISO)
	bash ./tests/musl_busybox_envshow_smoke.sh $(ISO)

dynlinkrealappsmoke: $(ISO)
	bash ./tests/dynlink_realapp_smoke.sh $(ISO)

vmsyscallsmoke: $(ISO)
	bash ./tests/vm_syscall_smoke.sh $(ISO)

cowmprotectsmoke: $(ISO)
	bash ./tests/cow_mprotect_smoke.sh $(ISO)

timesyscallsmoke: $(ISO)
	bash ./tests/time_syscall_smoke.sh $(ISO)

signalsyscallsmoke: $(ISO)
	bash ./tests/signal_syscall_smoke.sh $(ISO)

ftruncsavesmoke: $(ISO)
	bash ./tests/ftruncate_save_smoke.sh $(ISO)

preadpwritesmoke: $(ISO)
	bash ./tests/pread_pwrite_smoke.sh $(ISO)

xv6sparsesmoke: $(ISO)
	bash ./tests/xv6_sparse_smoke.sh $(ISO)

xv6reclaimsmoke: $(ISO)
	bash ./tests/xv6_reclaim_smoke.sh $(ISO)

xv6largewritesmoke: $(ISO)
	bash ./tests/xv6_largewrite_smoke.sh $(ISO)

virtionetirqsmoke: $(ISO)
	bash ./tests/virtio_net_irq_smoke.sh $(ISO)

virtioblkinflightsmoke: $(ISO)
	bash ./tests/virtio_blk_inflight_smoke.sh $(ISO)

virtioq35smoke: $(ISO)
	bash ./tests/virtio_q35_msix_smoke.sh $(ISO)

irqbottomhalfstresssmoke: $(ISO)
	bash ./tests/irq_bottom_half_stress_smoke.sh $(ISO)

irqbottomhalfsmpstresssmoke: $(ISO)
	bash ./tests/irq_bottom_half_smp_stress_smoke.sh $(ISO)

finalsmokesuite:
	bash ./tests/final_smoke_suite.sh

nativekernelbuildsmoke: $(ISO)
	bash ./tests/native_kernel_build_smoke.sh $(ISO)

nativekernelbootsmoke: $(ISO)
	bash ./tests/native_kernel_boot_smoke.sh $(ISO)

pythonnumpysmoke: $(ISO)
	bash ./tests/python_numpy_smoke.sh $(ISO)

smprun: $(ISO)
	bash tests/run_qemu_stdio.sh \
		-smp 2

persistsmprun: ROOTFS_REBUILD=0
persistsmprun: $(ISO) $(ROOTFS_IMG)
	bash tests/run_qemu_stdio.sh \
		$(ROOTFS_VBLK_ARGS) \
		-smp 2

smp4run: $(ISO)
	bash tests/run_qemu_stdio.sh \
		-smp 4

netrun: $(ISO)
	bash tests/run_qemu_stdio.sh \
		-netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=udp::12345-:12345,hostfwd=udp::12346-:12346 \
		-device virtio-net-pci,netdev=net0

persistnetrun: ROOTFS_REBUILD=0
persistnetrun: $(ISO) $(ROOTFS_IMG)
	bash tests/run_qemu_stdio.sh \
		$(ROOTFS_VBLK_ARGS) \
		-netdev user,id=net0,hostfwd=tcp::8080-:8080,hostfwd=udp::12345-:12345,hostfwd=udp::12346-:12346 \
		-device virtio-net-pci,netdev=net0

doommsulrun: $(ISO)
	bash tests/run_doom_musl.sh

doommuslrun: doommsulrun

usb-img:
	@if [ ! -f $(USB_IMG) ]; then \
		echo "Creating $(USB_IMG) (256M)"; \
		truncate -s 256M $(USB_IMG); \
	fi
	@python3 scripts/make_usb_img.py $(USB_IMG)
	@printf 'OrthOS USB TEST BLOCK 0002\nREAD(10) verification pattern\n' | \
		dd of=$(USB_IMG) bs=512 seek=2 conv=notrunc status=none
	@printf 'OrthOS USB TEST BLOCK 0003\nThird sector marker\n' | \
		dd of=$(USB_IMG) bs=512 seek=3 conv=notrunc status=none

usb: $(ISO) usb-img
	bash tests/run_qemu_stdio.sh \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=usbdisk,file=$(USB_IMG),format=raw \
		-device usb-storage,bus=xhci.0,drive=usbdisk

clean:
	rm -rf $(BUILD_DIR) $(KERNEL_ELF) $(USER_ELF) $(EXEC_ELF) $(PIPE_TEST_ELF) $(SH_ELF) $(GCC_ELF) $(LOOP_ELF) $(ISO)
	rm -f $(MUSL_USER_ELF) $(MUSL_SH_ELF)
	rm -f user/*.elf
	rm -rf iso_root
	rm -f rootfs.tar
	$(MAKE) -C Limine clean

-include $(DEPS)
