#!/bin/bash
# dup / fork した fd が offset を共有するか (Linux の open file description) の検証。
#
# Linux では dup / fork で作った fd は同じ open file description を指すので、
# 片方で読み書きすると、もう片方から見た位置も進む。offset が fd ごとの写しに
# なっていると、この性質が崩れる。
#
#   riscv64  kernel/riscv64/fs.c の fs_clone_fd() は *dst = *src の丸ごと複製。
#            offset は fd ごとに独立したまま (日報2026-08-03 の A 案は
#            size を inode から取り直すだけで、offset は手つかず)
#   x86      kernel/fs.c は fd->file (fs_file_t) を共有するので offset も共有
#
# 探針 user/riscv64_offset_probe.c の期待値そのものは、ホスト Linux で
#   gcc -DTMPPATH='"/tmp/offsprobe.tmp"' user/riscv64_offset_probe.c
# を走らせて 5/5 ok になることを確かめてある (測定器の目盛りを先に較正する)。
#
#   make riscv64-offset-smoke がビルドとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-riscv64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-riscv64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-riscv64
fi
if [ -z "$QEMU_BIN" ] && [ -x /usr/local/bin/qemu-system-riscv64 ]; then
    QEMU_BIN=/usr/local/bin/qemu-system-riscv64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-riscv64 not found" >&2
    exit 1
fi

FW_PATH=""
for c in /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
         /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
         /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin; do
    if [ -f "$c" ]; then FW_PATH="$c"; break; fi
done
if [ -z "$FW_PATH" ]; then
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SMP_CPUS="${SMP_CPUS:-1}"
SERIAL_LOG=LOGs/riscv64-offset-serial.log
ROOTFS_IMG=out/rootfs-riscv64-xv6.img
rm -f "$SERIAL_LOG"

if [ ! -f "$ROOTFS_IMG" ]; then
    echo "$ROOTFS_IMG が無い。'make riscv64-rootfs' で作ること" >&2
    echo "(このターゲットは書き込み可能な / を要る。埋め込み root だけでは足りない)" >&2
    exit 1
fi

# 探針は / にファイルを作って消す。元のイメージを汚さないよう複製に対して回す。
# out/rootfs-gcc-selfhost.img を潰した事故 (日報2026-08-03) と同じ形を避ける
WORK="$(mktemp -d)"
WORK_IMG="$WORK/rootfs-offset.img"
cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT
cp "$ROOTFS_IMG" "$WORK_IMG"

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp "$SMP_CPUS" \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -drive "if=none,id=rootfs,file=$WORK_IMG,format=raw" \
    -device virtio-blk-device,drive=rootfs \
    -display none \
    -serial stdio \
    -monitor none < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..60}; do
    if grep -aq "OFFS-PROBE-OK\|OFFS-PROBE-BAD" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V offset Serial Output ---"
grep -a "OFFS" "$SERIAL_LOG" || true
echo "-----------------------------------"

grep -aq "OFFS-PROBE-START" "$SERIAL_LOG"
# 1 件でも期待と違えば探針側が BAD を出す
! grep -aq " BAD" "$SERIAL_LOG"
grep -aq "OFFS-PROBE-OK" "$SERIAL_LOG"

echo "riscv64 offset smoke test: PASS (smp=$SMP_CPUS)"
