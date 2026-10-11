#!/bin/bash
# 段階 7: F/D レジスタがタスク切り替えを跨いで保たれるかの検査 (QEMU virt)。
#
# busybox ash を対話シェルとして起動し、/bin/fpprobe (user/riscv64-bin/fpprobe.c)
# を走らせる。hart 数より多く fork し、各ワーカーが「途中で取り上げられた」
# 回を数えるので、切り替えを跨がずに素通りしたかどうかも分かる。
#
#   make riscv64-fp-smoke              (1 hart と 4 hart の両方)
#   KERNEL=... SMP_CPUS=4 bash tests/riscv64_fp_smoke.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-riscv64 2>/dev/null || true)"
[ -n "$QEMU_BIN" ] || { echo "qemu-system-riscv64 not found" >&2; exit 1; }
FW_PATH=""
for f in /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
         /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
         /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin; do
    [ -f "$f" ] && { FW_PATH=$f; break; }
done
[ -n "$FW_PATH" ] || { echo "OpenSBI firmware not found" >&2; exit 1; }

KERNEL=${KERNEL:-out/kernel-riscv64.elf}
SMP=${SMP_CPUS:-1}
ROOTFS_IMG=out/rootfs-riscv64-xv6.img
LOG=LOGs/riscv64-fp-serial-smp$SMP.log
rm -f "$LOG"
# hart 数より多く fork する。持ち時間は 5 ティック (500 ms) なので、
# 1 本が数秒走るだけの rounds x spin にする
NPROC=${FP_NPROC:-$((SMP * 2 + 2))}
ARGS="$NPROC ${FP_ROUNDS:-30} ${FP_SPIN:-5000000}"

(
    sleep 8
    printf 'fpprobe %s; echo fp-exit=$?\n' "$ARGS"
    sleep "${FP_WAIT:-150}"
    printf 'exit\n'
    sleep 2
) | "$QEMU_BIN" -machine virt -cpu rv64 -m 512M -smp "$SMP" -bios "$FW_PATH" \
    -kernel "$KERNEL" -display none -serial stdio -monitor none \
    -drive "file=$ROOTFS_IMG,if=none,format=raw,id=vblk0" -device virtio-blk-device,drive=vblk0 \
    > "$LOG" 2>&1 &
QEMU_PID=$!
cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 $(( ${FP_WAIT:-150} + 20 ))); do
    grep -aq "fp-exit=" "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 1
grep -aE "^FP-|fp-exit=" "$LOG" | grep -v "printf" || true

fail() { echo "*** riscv64 fp smoke (smp $SMP): $1" >&2; exit 1; }
grep -aq "^FP-ALL-OK" "$LOG" || fail "FP-ALL-OK が出ない"
if grep -aq "^FP-CLOBBERED" "$LOG"; then fail "FP レジスタが壊れた"; fi
# 1 本も取り上げられていなければ検査になっていない
grep -aE "^FP-WORKER .* preempted=[1-9]" "$LOG" >/dev/null || fail "どのワーカーも切り替えを跨いでいない"
echo "riscv64 fp smoke (smp $SMP): PASS"
