#!/bin/bash
# aarch64: **段が同じページを跨ぐ ELF** が読めるかの検査 (riscv64 / x86 版と同じねらい)。
#
# kernel/elf.c は 1 ページを 1 度しか貼らず、2 つ目の段では
# arch_vm_update_page_flags を呼ぶ。aarch64 のそれは**置き換える**ので、
# 和を渡さないと共有ページが .data の R+W になり、**そこの命令が実行できない。**
#
# 探針はディスクの /bin/straddle に置き、カーネルは
# AARCH64_INIT_PATH_VALUE=/bin/straddle でそこを最初のユーザープロセスにする。
#   make aarch64-straddle-smoke がビルドとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs out

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-aarch64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-aarch64 not found" >&2
    exit 1
fi

KERNEL=out/kernel-aarch64.elf
PROBE=out/aarch64-straddle-probe.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL" >&2; exit 1; }
[ -f "$PROBE" ]  || { echo "missing $PROBE" >&2; exit 1; }

# **通常のスモークとは別のディスクを使う。** out/rootfs-*.img には触らない
TEST_DISK=out/aarch64-straddle-disk.img
TEST_FSDIR=out/aarch64-straddle-fs
LOG=LOGs/aarch64-straddle-serial.log

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$TEST_FSDIR"
mkdir -p "$TEST_FSDIR/bin"
cp "$PROBE" "$TEST_FSDIR/bin/straddle"
# カーネルの起動時自己診断が中身まで照合する既知ファイル
# (入れないと fs selftest が BAD を出し、探針の失敗と紛らわしい)
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
rm -f "$TEST_DISK"
XV6FS_FSSIZE=4096 XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null

rm -f "$LOG" "$LOG.nocr"
"$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp 1 \
    -nographic \
    -drive "file=$TEST_DISK,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 \
    -kernel "$KERNEL" < /dev/null > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..60}; do
    if grep -aq "STRADDLE-OK\|STRADDLE-BAD\|STRADDLE-NOT-SHARED\|bootstrap user exit" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- AArch64 straddle Serial Output ---"
tail -30 "$LOG"
echo "--------------------------------------"

# 判定は CR を除いたコピーに当てる (tests/aarch64_musl_smoke.sh と同じ理由)
tr -d '\r' < "$LOG" > "$LOG.nocr"
CHECK_LOG="$LOG.nocr"

grep -aq "STRADDLE-START" "$CHECK_LOG"
# **前提が崩れていたら緑にしない**
if grep -aq "STRADDLE-NOT-SHARED" "$CHECK_LOG"; then
    echo "段が同じページに乗っていない。リンカ台本を見直すこと" >&2
    exit 1
fi
grep -aq "STRADDLE-OK" "$CHECK_LOG"
if grep -aq "STRADDLE-BAD" "$CHECK_LOG"; then
    echo "*** STRADDLE-BAD が出た" >&2
    exit 1
fi

echo "aarch64 straddle smoke test: PASS"
