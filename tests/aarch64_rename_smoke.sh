#!/bin/bash
# rename(2) の検査。
#
# **番号がアーキで違うところで落ちていた。**aarch64 の musl は renameat(38)
# を出すが、カーネルは renameat2(276) しか見ておらず `rename()` が丸ごと
# ENOSYS を返していた。configure / libtool / move-if-change は rename を
# 何千回も踏むので、欠けていると「なぜか途中で止まる」形でしか出ない。
#
# **`mv` で確かめてはいけない。**busybox の mv は rename が失敗すると
# copy+unlink に退くので、rename が壊れていても mv は成功する。
# probe は rename(2) を直に呼び、戻り値と errno をそのまま出す。
#
# 実行前に probe と init パスを差し替えたカーネルが要る:
#   make aarch64-rename-smoke  がまとめてやる
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-aarch64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-aarch64 not found" >&2
    exit 1
fi

KERNEL=out/kernel-aarch64.elf
PROBE=out/aarch64-rename-probe.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-rename-smoke')" >&2; exit 1; }
[ -f "$PROBE" ]  || { echo "missing $PROBE ('make aarch64-rename-probe')" >&2; exit 1; }

# **通常のスモークとは別のディスクを使う。** out/rootfs-*.img には触らない
TEST_DISK=out/aarch64-rename-disk.img
TEST_FSDIR=out/aarch64-rename-fs
LOG=LOGs/aarch64-rename-serial.log

XV6FS_TEST_BLOCKS=16384          # 1KB ブロック x 16384 = 16MB

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 否定判定は必ずこの関数を通す。`! grep` は set -e の対象外で素通りする
# (日報2026-08-09 追9-6)
must_not() {   # $1 = 固定文字列, $2 = ログ, $3 = 説明 (任意)
    if grep -aqF -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た: $1  ($2)" >&2
        [ -n "${3:-}" ] && echo "*** $3" >&2
        grep -aF -- "$1" "$2" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

make_test_disk() {
    mkdir -p out
    rm -rf "$TEST_FSDIR"
    mkdir -p "$TEST_FSDIR/bin" "$TEST_FSDIR/tmp"
    cp "$PROBE" "$TEST_FSDIR/bin/rename-probe"
    # **カーネルの起動時自己診断が中身まで照合する既知ファイル。**
    # 入れないと fs selftest が read file : BAD を出す (probe の失敗と
    # 紛らわしいので、ディスクの都合で BAD を出させない)
    printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
    rm -f "$TEST_DISK"
    XV6FS_FSSIZE=$XV6FS_TEST_BLOCKS XV6FS_NINODES=256 \
        python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null
}

make_test_disk
rm -f "$LOG"

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

for _ in {1..120}; do
    if grep -aq "bootstrap user exit" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- AArch64 rename Serial Output ---"
cat "$LOG"
echo "------------------------------------"

# **打ち切りを黙って見逃さない。** マーカーが出ないまま止めた場合、
# 以降の判定は「中身が違う」ではなく「途中で切れた」で落ちる
if ! grep -aq "bootstrap user exit" "$LOG" 2>/dev/null; then
    echo "*** 実行が終わる前に打ち切られた ($LOG)" >&2
    echo "*** 最後の 5 行:" >&2
    tail -5 "$LOG" >&2
    exit 1
fi

# **ここが今回の本体。**rename が未実装だと ENOSYS がここに出る
if grep -aq "ENOSYS: syscall" "$LOG"; then
    echo "*** 未実装の syscall が呼ばれた:" >&2
    grep -a "ENOSYS: syscall" "$LOG" | sed 's/^/***   /' >&2
    exit 1
fi

# 判定は CR を除いたコピーに当てる (ONLCR で LF が CRLF になる)
tr -d '\r' < "$LOG" > "$LOG.nocr"
LOG="$LOG.nocr"

echo "--- rename(2) の判定 ---"
must_not "aarch64-fs-BAD" "$LOG"
must_not "aarch64-user-BAD" "$LOG"
grep -aq "exec      : /bin/rename-probe" "$LOG"
must_not "aarch64-init-BAD" "$LOG" "task_execve が失敗した (ELF が読めていない)"

grep -aqE "^RENAME-START$" "$LOG"

# probe が見た項目を 1 つずつ。**まとめて DONE だけ見ると、
# 個別に落ちたものが何だったか後から分からない**
for item in simple simple-body overwrite overwrite-body self self-body \
            missing dir-move dir-body dir-dotdot dir-notempty dir-loop \
            dir-onto-file file-onto-dir tmp-simple tmp-body crossdev; do
    grep -aqE "^RENAME-OK $item\$" "$LOG" || {
        echo "*** $item が通っていない" >&2
        grep -a "RENAME-BAD $item" "$LOG" | sed 's/^/***   /' >&2
        exit 1
    }
done

must_not "RENAME-BAD" "$LOG"
must_not "RENAME-FAILED" "$LOG"
grep -aqE "^RENAME-DONE$" "$LOG"
grep -aq "bootstrap user exit" "$LOG"

must_not "aarch64-exception-BAD" "$LOG"
must_not "PANIC" "$LOG"
must_not "xv6bio: disk" "$LOG"

echo "aarch64 rename smoke test: PASS"
