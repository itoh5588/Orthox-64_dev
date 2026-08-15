#!/bin/bash
# P4: busybox ash を aarch64 の EL0 で対話的に動かす。
#
# ディスクの /bin/ash を task_execve で読んで走らせ、標準入力からコマンドを
# 流し込んで結果を見る (riscv64 の riscv64_ash_smoke.sh に相当)。
#
# **riscv64 版より検査範囲が狭い。** あちらは rootfs (user/riscv64-bin,
# /etc/motd, orthinfo) を前提にした検査まで積んでいるが、こちらはまず
# 「ash が立ち上がって fork/exec/pipe/リダイレクトが通る」ところを固める。
# 通ったものから足していく。
#
# **applet はコピーで置く。** busybox は argv[0] のベース名で applet を選ぶ。
# xv6fs にシンボリックリンクを張る経路を当てにせず、同じ ELF を別名で置く
# (329KB x 数個。16MB のイメージに対して十分小さい)。
#
# 実行前に ash と init パスを差し替えたカーネルが要る:
#   make aarch64-ash-smoke  がまとめてやる
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
ASH=out/busybox-aarch64-musl.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-ash-smoke')" >&2; exit 1; }
[ -f "$ASH" ]    || { echo "missing $ASH ('make aarch64-busybox-musl')" >&2; exit 1; }

# **通常のスモークとは別のディスクを使う。** out/rootfs-*.img には触らない
TEST_DISK=out/aarch64-ash-disk.img
TEST_FSDIR=out/aarch64-ash-fs
LOG=LOGs/aarch64-ash-serial.log

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
    mkdir -p "$TEST_FSDIR/bin" "$TEST_FSDIR/tmp" "$TEST_FSDIR/etc"
    # ash 本体と、外部 exec で呼ぶ applet。**同じ ELF の別名コピー**。
    # **ash の組み込みで済むものも /bin に要る** — mkdir / rm / sleep は
    # 組み込みではないので、置き忘れると `rm: not found` になり、
    # 「fork/exec が壊れている」のと区別がつかない
    for applet in ash echo cat wc uname sort ls mkdir rm sleep grep; do
        cp "$ASH" "$TEST_FSDIR/bin/$applet"
    done
    printf 'hello from aarch64 xv6fs rootfs\n' > "$TEST_FSDIR/etc/motd"
    # **カーネルの起動時自己診断が中身まで照合する既知ファイル。**
    # 入れないと fs selftest が read file : BAD を出す (ash の失敗と
    # 紛らわしいので、ディスクの都合で BAD を出させない)
    printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
    rm -f "$TEST_DISK"
    XV6FS_FSSIZE=$XV6FS_TEST_BLOCKS XV6FS_NINODES=256 \
        python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null
}

make_test_disk
rm -f "$LOG"

# 送信側の sleep 合計は約 60 秒。下の待ちループはそれより十分大きく取ること
(
    sleep 10
    # ash が立ち上がってプロンプトを出しているか
    printf 'echo interactive-ok\n'
    sleep 2
    printf 'pwd\n'
    sleep 2
    # 変数展開
    printf 'x=42; echo val=$x\n'
    sleep 2
    # 組み込み applet (fork せずに済む経路)
    printf 'uname -m\n'
    sleep 3
    # **外部 exec。** fork + execve でディスクの ELF を読む
    printf '/bin/echo external-exec-ok\n'
    sleep 4
    # パイプ (fork 2 つ + pipe)
    printf 'echo a b c | wc\n'
    sleep 4
    # 読み込みリダイレクト
    printf 'cat /etc/motd\n'
    sleep 3
    # 書き込みリダイレクト
    printf 'echo redirect-ok > /w.txt\n'
    sleep 3
    printf 'cat /w.txt\n'
    sleep 3
    # 追記
    printf 'echo append-ok >> /w.txt\n'
    sleep 3
    printf 'cat /w.txt | wc -l\n'
    sleep 4
    # ディレクトリ操作
    printf 'mkdir /d 2>/dev/null; test -d /d && echo mkdir-ok\n'
    sleep 4
    # unlink
    printf 'rm /w.txt; cat /w.txt || echo unlink-ok\n'
    sleep 4
    # バックグラウンドジョブ (子が 1 度だけ走ること)
    printf 'BG=ok; /bin/echo bg-job-$BG &\n'
    sleep 4
    printf 'echo after-bg-ok\n'
    sleep 3
    # 算術展開 (CONFIG_FEATURE_SH_MATH)
    printf 'i=6; echo math=$((i*7+0))\n'
    sleep 3
    # nanosleep のタイマー起床。ここで止まるなら寝たきり
    printf 'sleep 1; echo sleep-ok\n'
    sleep 5
    printf 'exit\n'
    sleep 3
) | "$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp 1 \
    -display none \
    -serial stdio \
    -monitor none \
    -drive "file=$TEST_DISK,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 \
    -kernel "$KERNEL" > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..150}; do
    if grep -aq "bootstrap user exit" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- AArch64 ash Serial Output ---"
cat "$LOG"
echo "---------------------------------"

echo "--- P4 の判定 (ash) ---"

# **打ち切りを黙って見逃さない。** マーカーが出ないまま止めた場合、
# 以降の判定は「中身が違う」ではなく「途中で切れた」で落ちる
grep -aq "bootstrap user exit" "$LOG" || {
    echo "*** 'bootstrap user exit' が出ないまま打ち切った (ash が exit まで届いていない)" >&2
    exit 1
}

# カーネル側が P1 まで健全に来ていること
grep -aq "aarch64-fs-ok" "$LOG"
must_not "aarch64-fs-BAD"   "$LOG"
must_not "aarch64-init-BAD" "$LOG" "execve が失敗している (/bin/ash がディスクに無いか ELF が読めない)"
must_not "PANIC"            "$LOG"

# ash が対話モードで立ち上がったこと
grep -aq "built-in shell (ash)" "$LOG"
grep -aq "interactive-ok"  "$LOG"
grep -aq "^/"              "$LOG"          # pwd
grep -aq "val=42"          "$LOG"          # 変数展開
grep -aq "^aarch64$"       "$LOG"          # uname -m

# fork + execve でディスクの ELF を読む
grep -aq "external-exec-ok" "$LOG"

# パイプ (echo a b c | wc → 1 行 3 語 6 バイト)
grep -aqE "1 +3 +6" "$LOG"

# リダイレクト (読み / 書き / 追記)
grep -aq "hello from aarch64 xv6fs rootfs" "$LOG"
grep -aq "redirect-ok" "$LOG"
grep -aqE "^ *2$" "$LOG"                   # cat /w.txt | wc -l → 2 行 (追記が効いている)

# ディレクトリと unlink
grep -aq "mkdir-ok"  "$LOG"
grep -aq "unlink-ok" "$LOG"

# バックグラウンドジョブ: 子が 1 度だけ実行されること
grep -aq "bg-job-ok" "$LOG"
[ "$(grep -ac "bg-job-ok" "$LOG")" = "1" ]
grep -aq "after-bg-ok" "$LOG"

# 算術展開と nanosleep
grep -aq "math=42"  "$LOG"
grep -aq "sleep-ok" "$LOG"

echo "aarch64 ash smoke test: PASS"
