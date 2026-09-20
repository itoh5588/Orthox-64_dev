#!/bin/bash
# fork の CoW を 4 CPU で叩く (2026-09-19)。x86_64 版。
#
# user/cowstress.c を x86 の musl で組み、worker 8 本がそれぞれ fork を
# 繰り返して、親子が同じページを別の CPU で同時に写す。**中身が混ざらない
# こと** (cowstress: PASS) と、カーネルが止まらないことを見る。
# 子の半分のページは read(2) でカーネルに書かせるので、カーネルモードの
# CoW フォルト (CR0.WP) もここで通る。
# aarch64 版は tests/aarch64_cowstress_smoke.sh、riscv64 版は
# tests/riscv64_cowstress_smoke.sh。
#
# **rootfs は軽い方 (rootfs-lite.img) を使う (2026-09-20)。**Limine は ISO を
# CD-ROM 経由で丸ごと読むので、イメージの大きさがそのままブート時間になる
# (実測 約 6.4 MB/s)。320MB の rootfs.img だとブートだけで 53.6 秒かかるが、
# 96MB の軽い方なら 16 秒で済む。**軽い方は /etc/bootcmd が空なので、
# ゲスト内カーネルビルドは走らない** (cowstress には要らない。訳は Makefile の
# ROOTFS_LITE_IMG の定義)。rootfs.img 自体には触らないので /kbuild も消えない。
# cowstress は Limine のモジュールとして ISO に足し、カーネルがモジュールを
# パスの末尾一致でファイルとして見せるのを使って /boot/cowstress.elf で
# 起動する。ISO はここで組む (x86_kernel_smoke.sh と同じ手順)。
#
#   make x86-cowstress-smoke
#   SMP_CPUS で CPU 数を変えられる (既定 4)
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
for f in kernel.elf user/sh.elf user/cowstress.elf rootfs-lite.img iso/limine.conf; do
    if [ ! -f "$f" ]; then
        echo "missing $f" >&2
        exit 1
    fi
done

SMP_CPUS="${SMP_CPUS:-4}"
SERIAL_LOG=LOGs/x86-cowstress-serial.log
WORK="$(mktemp -d)"
ISO="$WORK/x86-cowstress-smoke.iso"
rm -f "$SERIAL_LOG" "$SERIAL_LOG.nocr"

cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/iso_root/boot/limine" "$WORK/iso_root/EFI/BOOT"
cp kernel.elf "$WORK/iso_root/boot/kernel.elf"
cp user/sh.elf "$WORK/iso_root/boot/sh.elf"
cp rootfs-lite.img "$WORK/iso_root/boot/rootfs.img"
cp user/cowstress.elf "$WORK/iso_root/boot/cowstress.elf"
# カーネルはモジュールを名前の末尾一致で探す (kernel/x86_64/init.c の
# find_module_by_suffix) ので、足すだけで sh.elf / rootfs.img には響かない
{
    cat iso/limine.conf
    echo "    module_path: boot():/boot/cowstress.elf"
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
    printf '/boot/cowstress.elf 8 50\n'
    wait_log "cowstress: (PASS|FAIL)" 300 || true
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

wait_log "cowstress: (PASS|FAIL)|#PF: CoW copy failed" 450 || true
sleep 1

echo "--- x86 cowstress Serial Output ---"
cat "$SERIAL_LOG"
echo "-----------------------------------"

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
grep -aqE "cowstress: start workers=" "$LOG"
must_not "cowstress: FAIL" "CoW で親子の中身が混ざった / fork・wait が失敗した"
must_not "#PF: CoW copy failed" "CoW の写し先のページが取れなかった"
grep -aqE "cowstress: PASS workers=" "$LOG"

echo "x86 cowstress smoke test: PASS (smp=$SMP_CPUS)"
