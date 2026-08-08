#!/bin/bash
# Orthox riscv64 の中でカーネルを再ビルドするためのソースツリーを組み立てる。
# x86 の rootfs/src/kernel-build に相当するものを riscv64 向けに作る。
#
#   出力: build/riscv64-native-src/  → rootfs の /src/kernel-build に入る
#
# Orthox 上での使い方:
#   cd /src/kernel-build && make
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/riscv64-native-src}"
# 途中で cd するので絶対パスにしておく
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

rm -rf "$OUT"
mkdir -p "$OUT/kernel/riscv64" "$OUT/include" "$OUT/scripts" "$OUT/out"

# macOS の AppleDouble (._foo.c) は中身がバイナリなので必ず除外する。
# ソース配下に 900 個近くあり、混ざるとビルドが壊れる。
copy_srcs() {  # $1=コピー元ディレクトリ $2=コピー先
  find "$1" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
       ! -name '._*' -exec cp {} "$2/" \;
}

copy_srcs "$ROOT/kernel"          "$OUT/kernel"
copy_srcs "$ROOT/kernel/riscv64"  "$OUT/kernel/riscv64"

# include/ は arch 別サブディレクトリも要る
( cd "$ROOT/include" && find . -type f -name '*.h' ! -name '._*' -print0 ) \
  | ( cd "$ROOT/include" && xargs -0 -I{} cp --parents {} "$OUT/include/" )

cp "$ROOT/scripts/kernel-riscv64.ld" "$OUT/scripts/"

# 埋め込む bootstrap user。objcopy が作るシンボル名は **パスから決まる**
# (_binary_out_bootstrap_user_riscv64_elf_start) ので、Orthox 上でも
# 相対パス out/bootstrap-user-riscv64.elf のまま objcopy すること。
if [ -f "$ROOT/out/bootstrap-user-riscv64.elf" ]; then
  cp "$ROOT/out/bootstrap-user-riscv64.elf" "$OUT/out/"
else
  echo "warning: out/bootstrap-user-riscv64.elf が無い。先に make riscv64-kernel を通すこと" >&2
fi

cat > "$OUT/Makefile" <<'MAKEFILE_EOF'
# Orthox riscv64 カーネル ネイティブビルド
# Orthox 上で実行:  cd /src/kernel-build && make
#
# ホスト側の Makefile と違い clang ではなく Orthox 上の gcc 4.7.4 を使う。
# /bin/sh は busybox ash なので for ループも使える。

SRCDIR ?= /src/kernel-build
BUILD  ?= /kbuild
OUTPUT ?= /kernel-riscv64.elf

# 絶対パスで指定する。PATH 頭の /bin には無いので、名前だけだと
# 1 コマンドごとに /bin/gcc への exec 失敗が 1 回入る (動くが遅く、ログも汚れる)
CC      = /usr/bin/gcc
OBJCOPY = /usr/bin/objcopy
LD      = /usr/bin/ld

# ホスト側の RISCV64_CFLAGS と同じ (clang 専用の --target= だけ落とす)。
# -std=gnu1x: 4.7.4 に c11 の綴りは無い。渡さないと既定 C89 で大量に落ちる。
CFLAGS = -std=gnu1x -march=rv64gc -mabi=lp64 -ffreestanding \
	 -fno-stack-protector -fno-stack-check -fno-lto -fno-pie \
	 -mcmodel=medany -O2 -I$(SRCDIR)/include

LDFLAGS = -nostdlib -static -m elf64lriscv -T $(SRCDIR)/scripts/kernel-riscv64.ld

RISCV64_C_SRCS = \
	kernel/riscv64/boot.c kernel/riscv64/bootstrap_user.c kernel/riscv64/elf.c \
	kernel/riscv64/entry.c kernel/riscv64/fs.c kernel/riscv64/net_socket.c \
	kernel/riscv64/plic.c kernel/riscv64/pmm.c kernel/riscv64/runtime.c \
	kernel/riscv64/smp.c kernel/riscv64/task.c kernel/riscv64/trap.c \
	kernel/riscv64/syscall.c kernel/riscv64/virtio_blk_mmio.c kernel/riscv64/vm.c

SHARED_C_SRCS = \
	kernel/task.c kernel/task_exec.c kernel/task_fork.c kernel/sched.c \
	kernel/wait.c kernel/elf.c kernel/cstring.c kernel/cstdio.c \
	kernel/storage.c kernel/xv6bio.c kernel/xv6log.c kernel/xv6fs.c

ASM_SRCS = kernel/riscv64/start.S kernel/riscv64/trap.S kernel/riscv64/entry.S

C_OBJS   = $(patsubst %.c,$(BUILD)/%.o,$(RISCV64_C_SRCS) $(SHARED_C_SRCS))
ASM_OBJS = $(patsubst %.S,$(BUILD)/%_asm.o,$(ASM_SRCS))
BLOB_OBJ = $(BUILD)/bootstrap_user_blob.o
OBJS     = $(C_OBJS) $(ASM_OBJS) $(BLOB_OBJ)

all: $(OUTPUT)

$(OUTPUT): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo "native-kernel-build-ok $@"

$(BUILD)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# シンボル名 _binary_out_bootstrap_user_riscv64_elf_* は **パスから決まる**ので
# SRCDIR に cd して相対パスのまま objcopy すること
$(BLOB_OBJ): $(SRCDIR)/out/bootstrap-user-riscv64.elf
	@mkdir -p $(dir $@)
	cd $(SRCDIR) && $(OBJCOPY) -I binary -O elf64-littleriscv \
	    out/bootstrap-user-riscv64.elf $@

clean:
	rm -rf $(BUILD) $(OUTPUT)

.PHONY: all clean
MAKEFILE_EOF

echo "組み立て完了: $OUT"
du -sh "$OUT"
find "$OUT" -name '*.c' | wc -l | sed 's/^/  .c ファイル: /'
find "$OUT" -name '*.h' | wc -l | sed 's/^/  .h ファイル: /'
