#!/bin/bash
# x86_64: **段が同じページを跨ぐ ELF** が読めるかの検査 (riscv64 版と同じねらい)。
#
# kernel/elf.c は 1 ページを 1 度しか貼らず、2 つ目の段では
# arch_vm_update_page_flags を呼ぶ。x86 のそれは 2026-09-12 まで**何もしない
# スタブ**だったので、共有ページには先に触った段 (.text の R+X) が残り、
# **そのページのデータに書けなかった。**
#
# rootfs.img は要らない。kernel/x86_64/init.c は Limine モジュールのうち
# パスが "sh.elf" で終わるものを elf_load して最初のユーザータスクにするので、
# **探針をその位置に置く。**
#   make x86-straddle-smoke がビルドとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "qemu-system-x86_64 not found" >&2; exit 1; }
command -v xorriso >/dev/null 2>&1 || { echo "xorriso not found" >&2; exit 1; }
for f in kernel.elf out/x86-straddle-probe.elf; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done

SERIAL_LOG=LOGs/x86-straddle-serial.log
WORK="$(mktemp -d)"
rm -f "$SERIAL_LOG" "$SERIAL_LOG.nocr"

cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/iso_root/boot/limine" "$WORK/iso_root/EFI/BOOT"
cp kernel.elf "$WORK/iso_root/boot/kernel.elf"
# **探針を sh.elf の位置に置く。**init.c はパスの末尾で拾う
cp out/x86-straddle-probe.elf "$WORK/iso_root/boot/sh.elf"
# 既定の iso/limine.conf は rootfs.img も並べているが、ここでは要らない
cat > "$WORK/iso_root/boot/limine/limine.conf" <<'CONF'
timeout: 0

/Orthox-64 straddle probe
    protocol: limine
    path: boot():/boot/kernel.elf
    module_path: boot():/boot/sh.elf
CONF
cp Limine/limine-bios.sys Limine/limine-bios-cd.bin Limine/limine-uefi-cd.bin \
   "$WORK/iso_root/boot/limine/"
cp Limine/BOOTX64.EFI Limine/BOOTIA32.EFI "$WORK/iso_root/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$WORK/iso_root" -o "$WORK/x86-straddle.iso" >/dev/null 2>&1

qemu-system-x86_64 \
    -machine pc \
    -cpu max \
    -m 2G \
    -smp 1 \
    -cdrom "$WORK/x86-straddle.iso" \
    -boot d \
    -display none \
    -serial stdio \
    -monitor none < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..60}; do
    if grep -aq "STRADDLE-OK\|STRADDLE-BAD\|STRADDLE-NOT-SHARED" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- x86 straddle Serial Output ---"
tail -40 "$SERIAL_LOG"
echo "----------------------------------"

# 判定は CR を除いたコピーに当てる (tests/aarch64_musl_smoke.sh と同じ理由)
tr -d '\r' < "$SERIAL_LOG" > "$SERIAL_LOG.nocr"
CHECK_LOG="$SERIAL_LOG.nocr"

grep -aq "STRADDLE-START" "$CHECK_LOG"
# **前提が崩れていたら緑にしない**
if grep -aq "STRADDLE-NOT-SHARED" "$CHECK_LOG"; then
    echo "段が同じページに乗っていない。リンカ台本を見直すこと" >&2
    exit 1
fi
grep -aq "STRADDLE-OK" "$CHECK_LOG"
! grep -aq "STRADDLE-BAD" "$CHECK_LOG"

echo "x86 straddle smoke test: PASS"
