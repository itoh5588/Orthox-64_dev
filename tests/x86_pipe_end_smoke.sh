#!/bin/bash
# x86_64: pipe / FIFO の端ごとの本数 (pipe_t の readers / writers) の回帰テスト。
#
# 直したもの:
#   kernel/fs.c    read の EOF 判定を writers==0 に、write に EPIPE を追加
#   kernel/sys_fs.c pselect の ready 判定も端ごとに
#
# 旧実装は ref_count ひとつ (= pipe を指す file object の総数) で EOF を
# 判定していた。同じ端を 2 回開くと合計が 3 になり、片方の端が全部閉じても
# ref_count<2 に届かない。FIFO は open ごとに参照が増えるのでこれを踏む。
# 匿名 pipe は読み端/書き端で別の file object を持ち、dup/fork は
# fs_file_t 側の ref を増やすだけなので、こちらは元から壊れていない。
#
# 逆確認の実測 (このスクリプトで取った):
#   EOF 判定だけ旧に戻す   -> fifoeof= が出ないまま read が返らない (ハング)
#   EPIPE 判定だけ外す     -> FAIL sigpipe write returned 4
#
# 使うもの:
#   /bin/pipeend_probe.elf  user/pipeend_probe.c
#
#   make x86-pipe-end-smoke がカーネルのビルドとあわせて実行する
#   SMP_CPUS で CPU 数を変えられる (既定 4)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "qemu-system-x86_64 not found" >&2
    exit 1
fi
if ! command -v xorriso >/dev/null 2>&1; then
    echo "xorriso not found" >&2
    exit 1
fi
for f in kernel.elf user/sh.elf rootfs.img iso/limine.conf; do
    if [ ! -f "$f" ]; then
        echo "missing $f" >&2
        exit 1
    fi
done

# イメージに探針が入っていないと、起動してから 60 秒待って
# 「grep が当たらない」という分かりにくい落ち方をする。先に見ておく
if ! python3 scripts/build_rootfs_xv6fs.py --stat /bin/pipeend_probe.elf rootfs.img >/dev/null 2>&1; then
    echo "rootfs.img に /bin/pipeend_probe.elf が無い。'make rootfs.img' で入れること" >&2
    exit 1
fi

SMP_CPUS="${SMP_CPUS:-4}"
SERIAL_LOG=LOGs/x86-pipe-end-serial.log
WORK="$(mktemp -d)"
ISO="$WORK/x86-pipe-end.iso"
rm -f "$SERIAL_LOG"

cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/iso_root/boot/limine" "$WORK/iso_root/EFI/BOOT"
cp kernel.elf "$WORK/iso_root/boot/kernel.elf"
cp user/sh.elf "$WORK/iso_root/boot/sh.elf"
cp rootfs.img "$WORK/iso_root/boot/rootfs.img"
cp iso/limine.conf "$WORK/iso_root/boot/limine/limine.conf"
cp Limine/limine-bios.sys Limine/limine-bios-cd.bin Limine/limine-uefi-cd.bin \
   "$WORK/iso_root/boot/limine/"
cp Limine/BOOTX64.EFI Limine/BOOTIA32.EFI "$WORK/iso_root/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$WORK/iso_root" -o "$ISO" >/dev/null 2>&1

# ブート直後に /etc/bootcmd のスモークが走るので、それが終わるまで待つ
(
    sleep 30
    printf '/bin/pipeend_probe.elf\n'
    sleep 30
    printf 'echo x86-pipe-end-done\n'
    sleep 5
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

for _ in {1..150}; do
    if grep -aq "x86-pipe-end-done" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- x86 pipe end Serial Output ---"
grep -a "PIPEEND" "$SERIAL_LOG" || true
echo "----------------------------------"

# シェルは 1 文字ずつエコーバックするので、grep は出力側にしか現れない
# 文字列に当てること (コマンド名はエコーで即座にログへ出る)
grep -aq "PIPEEND: dupeof=OK" "$SERIAL_LOG"      # 読み端を dup しても EOF が来る
grep -aq "PIPEEND: sigpipe=OK" "$SERIAL_LOG"     # 読み手全 close 後の write は EPIPE
grep -aq "PIPEEND: fifoeof=OK" "$SERIAL_LOG"     # FIFO を 2 本で開いても EOF が来る
grep -aq "PIPEEND: fifoepipe=OK" "$SERIAL_LOG"   # FIFO の読み手全 close で EPIPE
grep -aq "PIPEEND: PASS" "$SERIAL_LOG"

# ハングしていないこと。fifoeof の逆確認では read が返らず、ここが出ない
grep -aq "x86-pipe-end-done" "$SERIAL_LOG"

echo "x86 pipe end smoke test: PASS (smp=$SMP_CPUS)"
