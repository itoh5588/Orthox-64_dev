#!/bin/bash
# musl 静的リンクプログラム (riscv64-musl-probe) を /bootstrap-user として起動し、
# getcwd / open / fstat / read / mmap / fork / waitpid の動作を検証する。
# 実行前に以下で probe 埋め込みカーネルをビルドしておくこと:
#   make riscv64-kernel RISCV64_BOOTSTRAP_USER_SRC_ELF=out/riscv64-musl-probe.elf
# (make riscv64-musl-smoke がこの手順をまとめて行う)
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
if [ -f /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
elif [ -f /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
elif [ -f /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    # Debian/Ubuntu の qemu-system-misc はここに置く
    FW_PATH=/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SERIAL_LOG=LOGs/riscv64-musl-serial.log
rm -f "$SERIAL_LOG"

# rootfs があれば繋ぐ。probe の BIGWRITE (xv6fs のログ分割の退行検査) は
# 書き込める FS が要るので、無い構成では probe 側が静かに飛ばす。
ROOTFS_IMG=out/rootfs-riscv64-xv6.img
DRIVE_ARGS=()
if [ -f "$ROOTFS_IMG" ]; then
    DRIVE_ARGS=(-drive "file=$ROOTFS_IMG,if=none,format=raw,id=vblk0" -device virtio-blk-device,drive=vblk0)
fi

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 1 \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -monitor none "${DRIVE_ARGS[@]}" &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..20}; do
    if grep -q "bootstrap user exit" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

sleep 1

echo "--- RISC-V musl Serial Output ---"
cat "$SERIAL_LOG"
echo "---------------------------------"

grep -q "Orthox riscv64 early boot" "$SERIAL_LOG"
grep -q "sv39 satp enabled" "$SERIAL_LOG"
grep -q "Task system initialized." "$SERIAL_LOG"
# 自己テストが「通った」ことまで見る。ここを見ないと、カーネルが
# 「selftest: ... failed」を出しても素通りする (st_ino の退行検査で実際に踏んだ)。
grep -q "fs syscall selftest passed" "$SERIAL_LOG"
grep -q "MUSL:/" "$SERIAL_LOG"
grep -q "^ELF$" "$SERIAL_LOG"
grep -q "^MAP$" "$SERIAL_LOG"
grep -q "^CHILD$" "$SERIAL_LOG"
grep -q "^DONE$" "$SERIAL_LOG"
# clone の引数チェックが a2 以降を見ていないこと (見ていると ENOSYS で落ちる)
grep -q "^CLONEG-CHILD$" "$SERIAL_LOG"
grep -q "^CLONEG-OK$" "$SERIAL_LOG"
# 1 回で 200KB 書けること (xv6fs のログ分割が無いとカーネルパニックする)。
# rootfs を繋いだ構成でのみ出る。
if [ -f out/rootfs-riscv64-xv6.img ]; then
    grep -q "^BIGWRITE-OK$" "$SERIAL_LOG"
fi
grep -q "bootstrap user exit" "$SERIAL_LOG"

echo "riscv64 musl smoke test: PASS"
