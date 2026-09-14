#!/bin/bash
# x86_64: bootstrap task (カーネルが直接起こした最初のユーザータスク。
# ppid==0) が exit したときの経路を実機 (QEMU) で確かめる。
#
# kernel/sys_task.c の sys_exit は `!current || current->ppid == 0` を
# bootstrap task の exit として arch_halt_forever() へ落とす (別実装 29 組の
# exit/wait4 統合、2026-09-13)。aarch64/riscv64 は smoke の /bin/hello 経由で
# 日常的にこの分岐を踏んでいるが、x86 の bootstrap task は ash (shell) で、
# この環境の ash は対話プロンプトから exit を打っても標準入力を EOF にしても
# sys_exit を呼ばない制約があり (2026-09-13 に実機で確認)、x86 だけ未検証
# だった。
#
# rootfs.img は要らない。kernel/x86_64/init.c は Limine モジュールのうち
# パスが "sh.elf" で終わるものを elf_load して最初のユーザータスクにするので、
# 探針をその位置に置いて bootstrap task 自身として直接 exit(2) を呼ばせる
# (tests/x86_straddle_smoke.sh と同じ手法)。
#   make x86-bootstrap-exit-smoke がビルドとあわせて実行する
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "qemu-system-x86_64 not found" >&2; exit 1; }
command -v xorriso >/dev/null 2>&1 || { echo "xorriso not found" >&2; exit 1; }
for f in kernel.elf out/x86-bootstrap-exit-probe.elf; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done

SERIAL_LOG=LOGs/x86-bootstrap-exit-serial.log
WORK="$(mktemp -d)"
rm -f "$SERIAL_LOG"

cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/iso_root/boot/limine" "$WORK/iso_root/EFI/BOOT"
cp kernel.elf "$WORK/iso_root/boot/kernel.elf"
# **探針を sh.elf の位置に置く。**init.c はパスの末尾で拾う
cp out/x86-bootstrap-exit-probe.elf "$WORK/iso_root/boot/sh.elf"
cat > "$WORK/iso_root/boot/limine/limine.conf" <<'CONF'
timeout: 0

/Orthox-64 bootstrap-exit probe
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
    "$WORK/iso_root" -o "$WORK/x86-bootstrap-exit.iso" >/dev/null 2>&1

qemu-system-x86_64 \
    -machine pc \
    -cpu max \
    -m 2G \
    -smp 1 \
    -no-reboot \
    -cdrom "$WORK/x86-bootstrap-exit.iso" \
    -boot d \
    -display none \
    -serial stdio \
    -monitor none < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..30}; do
    if grep -aq "bootstrap user exit\|BOOTSTRAP-EXIT-PROBE-RETURNED" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

# **止まったことそのものを見る。**arch_halt_forever は cli+hlt なので、
# メッセージが出た後は CPU 使用率が下がって QEMU プロセスは生き続ける
# (無限 zombie loop なら kernel_yield を回し続けて CPU 使用率が高いまま)。
# メッセージが出てから少し待ち、その間に新しい出力が増えない
# (=ハングでもクラッシュでもなく、狙いどおり止まっている) ことを確かめる
sleep 2
LINES_BEFORE=$(wc -l < "$SERIAL_LOG")
sleep 2
LINES_AFTER=$(wc -l < "$SERIAL_LOG")

echo "--- x86 bootstrap-exit Serial Output ---"
cat "$SERIAL_LOG"
echo "-----------------------------------------"

if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "QEMU が終了している (トリプルフォルト等でリブート/クラッシュした可能性)" >&2
    exit 1
fi

grep -aq "BOOTSTRAP-EXIT-PROBE-START" "$SERIAL_LOG"
grep -aq "bootstrap user exit" "$SERIAL_LOG"
# **戻ってきたら異常。**sys_exit の ppid==0 分岐は arch_halt_forever() を
# 呼んで戻らない想定
! grep -aq "BOOTSTRAP-EXIT-PROBE-RETURNED" "$SERIAL_LOG"

if [ "$LINES_AFTER" != "$LINES_BEFORE" ]; then
    echo "bootstrap user exit の後もログが増え続けている (hlt で止まっていない可能性)" >&2
    exit 1
fi

echo "x86 bootstrap-exit smoke test: PASS"
