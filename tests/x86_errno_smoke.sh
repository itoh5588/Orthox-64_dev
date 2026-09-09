#!/bin/bash
# x86_64: 失敗系 syscall の errno 検証。
#
# カーネルが「よく分からない失敗はとりあえず -1」を返していると、Linux の
# errno 規約では -1 = EPERM なので、ユーザーには "Operation not permitted"
# として出てくる。busybox は errno を見て挙動を変える箇所が多く (`rm -f` が
# 典型)、EPERM だと誤動作する。
#
# 検証のしかた: busybox は strerror(errno) をそのまま表示するので、
# **エラーメッセージの文字列そのものが assert になる**。
# macOS では x86 ユーザーランドを再ビルドできないが、既存の rootfs.img に
# busybox が入っているので、新しいカーネルと組み合わせれば実行検証できる。
#
# 限界: ここで見えるのは busybox が踏む経路だけで、kernel/fs.c の全経路では
# ない。riscv64 側は user/riscv64_errno_probe.c で 21 件を直接照合している。
#
#   make x86-errno-smoke がカーネルのビルドとあわせて実行する
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
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done

SMP_CPUS="${SMP_CPUS:-2}"
SERIAL_LOG=LOGs/x86-errno-serial.log
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
    "$WORK/iso_root" -o "$WORK/x86-errno.iso" >/dev/null 2>&1

# ブート直後は /etc/bootcmd のネイティブビルドが走るので、終わってから流す
(
    sleep 25
    printf 'cat /no-such-file\n'
    printf 'rmdir /no-such-dir\n'
    printf 'mkdir /no-such-dir/x\n'
    printf 'mkdir /etc\n'
    printf 'mv /no-such-file /x\n'
    printf 'rm /etc\n'
    printf 'rm -f /no-such-file\n'
    printf '/bin/staterrno.elf\n'
    printf 'echo x86-errno-d0ne\n'
    sleep 10
) | qemu-system-x86_64 \
    -machine pc \
    -cpu max \
    -m 2G \
    -smp "$SMP_CPUS" \
    -cdrom "$WORK/x86-errno.iso" \
    -boot d \
    -display none \
    -serial stdio \
    -monitor none > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..120}; do
    if grep -aq "x86-errno-d0ne" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

# シェルは 1 文字ずつエコーバックし、間に ESC[K を挟む。
# エラーメッセージは出力側にしか出ないので、エスケープを落としてから照合する
CLEAN="$WORK/clean.log"
tr -d '\r' < "$SERIAL_LOG" | sed 's/\x1b\[K//g' > "$CLEAN"

echo "--- x86 errno (抜粋) ---"
grep -aoE "(cat|rmdir|mkdir|mv|rm): .*(No such|File exists|directory|permitted).*|staterrno: rc=[-0-9]+ errno=[0-9]+" "$CLEAN" | sort -u
echo "------------------------"

fail=0
expect() {   # expect <説明> <正規表現>
    if grep -aqE "$2" "$CLEAN"; then
        echo "ok   : $1"
    else
        echo "BAD  : $1  (期待: $2)"
        fail=1
    fi
}

expect "存在しないパスの mkdir は ENOENT"   "mkdir: can't create directory '/no-such-dir/x': No such file or directory"
expect "存在しないパスの rmdir は ENOENT"   "rmdir: '/no-such-dir': No such file or directory"
expect "存在しないパスの mv は ENOENT"      "mv: can't rename '/no-such-file': No such file or directory"
expect "既存ディレクトリの mkdir は EEXIST" "mkdir: can't create directory '/etc': File exists"
expect "ディレクトリの rm は EISDIR"        "rm: '/etc' is a directory"
expect "stat の ENOENT"                     "staterrno: rc=-1 errno=2"

# 決め手: どのケースでも EPERM ("Operation not permitted") が出ないこと。
# -1 を返すとここに落ちてくるので、この 1 行が退行検出の本体になる
if grep -aq "Operation not permitted" "$CLEAN"; then
    echo "BAD  : EPERM が出ている (カーネルが -1 を返している)"
    grep -a "Operation not permitted" "$CLEAN" | sort -u
    fail=1
else
    echo "ok   : EPERM は出ていない"
fi

grep -aq "x86-errno-d0ne" "$CLEAN" || { echo "BAD  : 最後まで到達していない"; fail=1; }

[ "$fail" = "0" ] || { echo "x86 errno smoke test: FAIL"; exit 1; }
echo "x86 errno smoke test: PASS"
