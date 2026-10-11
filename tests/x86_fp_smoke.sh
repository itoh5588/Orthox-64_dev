#!/bin/bash
# SSE レジスタがタスク切り替え / fork / exec を跨いで正しく扱われるかの検査
# (2026-10-11)。x86_64 版。riscv64 は tests/riscv64_fp_smoke.sh、aarch64 は
# tests/aarch64_fp_smoke.sh。
#
# user/x86_fp_probe.c を x86 の musl で組み、Limine のモジュールとして ISO に
# 足して /boot/x86_fp_probe.elf で起動する (手順は x86_cowstress_smoke.sh と
# 同じ。rootfs は軽い方)。CPU 数より多く fork して切り替えを跨がせる。
#
#   make x86-fp-smoke
#   SMP_CPUS で CPU 数を変えられる (既定 1)。KERNEL で別のカーネルも試せる
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

for tool in qemu-system-x86_64 xorriso; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool not found" >&2
        exit 1
    fi
done
KERNEL="${KERNEL:-kernel.elf}"
for f in "$KERNEL" user/sh.elf user/x86_fp_probe.elf rootfs-lite.img iso/limine.conf; do
    if [ ! -f "$f" ]; then
        echo "missing $f" >&2
        exit 1
    fi
done

SMP_CPUS="${SMP_CPUS:-1}"
SERIAL_LOG=LOGs/x86-fp-serial-smp$SMP_CPUS.log
WORK="$(mktemp -d)"
ISO="$WORK/x86-fp-smoke.iso"
NPROC="${FP_NPROC:-$((SMP_CPUS * 2 + 2))}"
# 既定は yield しない計算ループ = タイマによる取り上げに任せる (FP_YIELD=1 で
# 塊ごとに sched_yield)。1 本が持ち時間 (5 ティック = 50 ms) を何度も超える長さにする
ARGS="$NPROC ${FP_ROUNDS:-12} ${FP_SPIN:-20000000} ${FP_YIELD:-0}"
rm -f "$SERIAL_LOG" "$SERIAL_LOG.nocr"

cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/iso_root/boot/limine" "$WORK/iso_root/EFI/BOOT"
cp "$KERNEL" "$WORK/iso_root/boot/kernel.elf"
cp user/sh.elf "$WORK/iso_root/boot/sh.elf"
cp rootfs-lite.img "$WORK/iso_root/boot/rootfs.img"
cp user/x86_fp_probe.elf "$WORK/iso_root/boot/x86_fp_probe.elf"
# カーネルはモジュールを名前の末尾一致で探す (kernel/x86_64/init.c の
# find_module_by_suffix) ので、足すだけで sh.elf / rootfs.img には響かない
{
    cat iso/limine.conf
    echo "    module_path: boot():/boot/x86_fp_probe.elf"
} > "$WORK/iso_root/boot/limine/limine.conf"
cp Limine/limine-bios.sys Limine/limine-bios-cd.bin Limine/limine-uefi-cd.bin \
   "$WORK/iso_root/boot/limine/"
cp Limine/BOOTX64.EFI Limine/BOOTIA32.EFI "$WORK/iso_root/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$WORK/iso_root" -o "$ISO" >/dev/null 2>&1

# シェルへの入力。シェルが起動するのをログで待ってから流す
wait_log() {   # $1 = 拡張正規表現, $2 = 秒
    for _ in $(seq 1 "$2"); do
        if grep -aqE -- "$1" "$SERIAL_LOG" 2>/dev/null; then
            return 0
        fi
        sleep 1
    done
    return 1
}
(
    # **待つのはシェルの起動 (2026-09-20)。**軽い rootfs は /etc/bootcmd が
    # 空でゲスト内ビルドを走らせないので、native-kernel-build-end は出ない
    wait_log "Welcome to Orthox-64 Shell" 120 || true
    sleep 2
    printf '/boot/x86_fp_probe.elf %s\n' "$ARGS"
    wait_log "^FP-(ALL-OK|FAIL)" "${FP_WAIT:-300}" || true
    sleep 2
) | qemu-system-x86_64 \
    -machine pc \
    -cpu max \
    -m 2G \
    -smp "$SMP_CPUS" \
    -cdrom "$ISO" \
    -boot d \
    -display none \
    -serial stdio \
    -monitor none > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

wait_log "^FP-(ALL-OK|FAIL)" "$(( ${FP_WAIT:-300} + 150 ))" || true
sleep 1

grep -aE "^FP-" "$SERIAL_LOG" | tr -d '\r' | head -40

# 判定は CR を除いたコピーに当てる。シェルは 1 文字ずつエコーバックするので、
# grep はコマンド名ではなく出力側にしか現れない文字列に当てること
tr -d '\r' < "$SERIAL_LOG" > "$SERIAL_LOG.nocr"
LOG="$SERIAL_LOG.nocr"

must_not() {   # $1 = 固定文字列, $2 = 説明
    if grep -aqF -- "$1" "$LOG"; then
        echo "*** 出てはいけないものが出た: $1" >&2
        echo "*** $2" >&2
        grep -aF -- "$1" "$LOG" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

grep -aq "Task system initialized" "$LOG"
grep -aqE "^FP-START" "$LOG"
must_not "FP-INHERIT-BAD" "子に親の古い SSE 状態が写った (fork が ctx の古い fxsave 領域を写している)"
grep -aqE "^FP-INHERIT-OK" "$LOG"
must_not "FP-EXEC-LEAKED" "exec 後に前の像の SSE 状態が見えた"
grep -aqE "^FP-EXEC-OK" "$LOG"
must_not "FP-CLOBBERED" "切り替えで SSE レジスタが壊れた"
grep -aqE "^FP-ALL-OK" "$LOG"
# 1 本も取り上げられていなければ検査になっていない
grep -aqE "^FP-WORKER .* preempted=[1-9]" "$LOG" || { echo "*** どのワーカーも切り替えを跨いでいない" >&2; exit 1; }

echo "x86 fp smoke test: PASS (smp=$SMP_CPUS)"
