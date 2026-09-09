MUSL_USER_CFLAGS = -target $(TARGET) -std=c11 -ffreestanding -fno-PIE -O2 -Wno-shift-op-parentheses \
	-I$(MUSL_SYSROOT)/include -Iinclude -Iports/BearSSL/inc -MMD -MP

$(USER_BUILD_DIR)/crt0.o: user/crt0.S
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscalls.o: user/syscalls.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/tls.o: user/tls.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

$(USER_BUILD_DIR)/syscall_wrap.o: user/syscall_wrap.S
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

USER_COMMON_OBJS = $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/tls.o $(USER_BUILD_DIR)/syscall_wrap.o

$(USER_BUILD_DIR)/%.o: user/%.c
	@mkdir -p $(@D)
	$(CC) $(MUSL_USER_CFLAGS) -c $< -o $@

# パターンルール: ほとんどの ELF はこれでビルド可能
user/%.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/%.o $(USER_COMMON_OBJS) $(LIBC)
	$(LD) $(USER_LDFLAGS) $^ -o $@
# 特殊な依存関係を持つもの
$(HTTPS_FETCH_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/httpsfetch.o $(USER_COMMON_OBJS) $(LIBC) $(BEARSSL_A)
	$(LD) $(USER_LDFLAGS) $^ -o $@
$(CC1_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/tls.o $(USER_BUILD_DIR)/syscall_wrap.o $(LIBC) ports/build_gcc_musl.sh
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_gcc_musl.sh
	cp ports/gcc-4.7.4/build-musl/gcc/cc1 $@

$(AS_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/tls.o $(USER_BUILD_DIR)/syscall_wrap.o $(LIBC)
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_binutils_musl.sh
	cp ports/binutils-2.26/binutils-2.26/build-musl/gas/as-new $@

$(LD_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/tls.o $(USER_BUILD_DIR)/syscall_wrap.o $(AS_MUSL_ELF) $(LIBC)
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_binutils_musl.sh
	cp ports/binutils-2.26/binutils-2.26/build-musl/ld/ld-new $@

$(MAKE_MUSL_ELF): $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/tls.o $(USER_BUILD_DIR)/syscall_wrap.o $(LIBC) ports/build_make_musl.sh ports/make-4.4.1-orthos.patch
	./ports/build_make_musl.sh $@

toolchain-musl: $(CC1_MUSL_ELF) $(AS_MUSL_ELF) $(LD_MUSL_ELF) $(MAKE_MUSL_ELF)

$(HELLO_DYN_ELF): user/hello_dyn.c ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIE $< -o $@

$(DYNLINK_LIB_A_SO): user/dynlink_lib_a.c ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIC -shared $< -Wl,-soname,libdyn_a.so -o $@

$(DYNLINK_LIB_B_SO): user/dynlink_lib_b.c $(DYNLINK_LIB_A_SO) ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIC -shared $< -Luser -ldyn_a -Wl,-soname,libdyn_b.so -o $@

$(DYNLINK_MULTI_TLS_ELF): user/dynlink_multi_tls.c $(DYNLINK_LIB_A_SO) $(DYNLINK_LIB_B_SO) ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIE $< -Luser -ldyn_b -ldyn_a -o $@

$(DYNLINK_PLUGIN_SO): user/dynlink_plugin.c $(DYNLINK_LIB_A_SO) $(DYNLINK_LIB_B_SO) ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIC -shared $< -Luser -ldyn_b -Wl,-soname,libdyn_plugin.so -o $@

$(BUILD_DIR)/musl/dynlink_cpp_runtime.o: user/dynlink_cpp_runtime.cc
	@mkdir -p $(@D)
	clang++ -target x86_64-linux-musl -ffreestanding -fPIC -fno-exceptions -fno-rtti -nostdinc -isystem $(MUSL_SYSROOT)/include -c $< -o $@

$(DYNLINK_CPP_SO): $(BUILD_DIR)/musl/dynlink_cpp_runtime.o ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -shared $< -Wl,-soname,libdyn_cpp.so -o $@

$(DYNLINK_DLOPEN_ELF): user/dynlink_dlopen.c $(DYNLINK_PLUGIN_SO) $(DYNLINK_CPP_SO) ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIE $< -ldl -o $@

$(DYNLINK_MALLOC_ELF): user/dynlink_malloc.c ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIE $< -o $@

$(BUSYBOX_ASH_DYN_ELF): ports/build_busybox_ash.sh ports/busybox-ash-dyn.config ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ORTHOS_BUSYBOX_DYNAMIC=1 ORTHOS_BUSYBOX_CONFIG=$(abspath ports/busybox-ash-dyn.config) ./ports/build_busybox_ash.sh $(abspath ports/busybox) $(abspath $(BUSYBOX_ASH_DYN_ELF))

$(GCC_DYN_ELF): user/gcc.c ports/musl-install/bin/musl-clang ports/musl-install/bin/ld.musl-clang $(USER_LIBDIR)/libc.so
	ports/musl-install/bin/musl-clang -fPIE $< -o $@

$(BUSYBOX_ASH_MUSL_ELF): $(USER_BUILD_DIR)/syscalls.o $(USER_BUILD_DIR)/tls.o $(USER_BUILD_DIR)/syscall_wrap.o ports/build_busybox_ash.sh ports/busybox-ash.config
	ORTHOS_SYSROOT=$(abspath $(MUSL_SYSROOT)) ORTHOS_INCLUDEDIR=$(abspath $(MUSL_SYSROOT))/include ORTHOS_LIBDIR=$(abspath $(MUSL_SYSROOT))/lib ORTHOS_CRT0=$(abspath $(USER_BUILD_DIR)/crt0.o) ORTHOS_SYSCALLS_O=$(abspath $(USER_BUILD_DIR)/syscalls.o) ORTHOS_SYSCALL_WRAP_O=$(abspath $(USER_BUILD_DIR)/syscall_wrap.o) ./ports/build_busybox_ash.sh $(abspath ports/busybox) $(abspath $(BUSYBOX_ASH_MUSL_ELF))

__busybox_ash_musl: $(BUSYBOX_ASH_MUSL_ELF)

__busybox_ash_musl_install: $(BUSYBOX_ASH_MUSL_ELF) $(AT_TEST_ELF)
	mkdir -p rootfs/bin
	# Install musl busybox and applets directly to rootfs/bin
	cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/busybox && chmod +x rootfs/bin/busybox
	for applet in $(BUSYBOX_ASH_APPLETS); do cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/$$applet && chmod +x rootfs/bin/$$applet; done
	cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/ash.musl && chmod +x rootfs/bin/ash.musl
	cat $(BUSYBOX_ASH_MUSL_ELF) > rootfs/bin/sh.musl && chmod +x rootfs/bin/sh.musl
	cat $(AT_TEST_ELF) > rootfs/bin/at_test.elf && chmod +x rootfs/bin/at_test.elf
