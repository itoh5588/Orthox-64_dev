#!/bin/bash
# P3-3: FP/SIMD が切り替えを跨いで保たれるかの検査。
#
# ディスクの /bin/fp-probe を task_execve で読んで走らせる (P1 と同じ方針で
# カーネルには埋め込まない)。riscv64 の riscv64_musl_smoke.sh に相当するが、
# **fork / clone は見ない** — aarch64 はまだ実装していないので P3 の領分。
#
# 実行前に probe と init パスを差し替えたカーネルが要る:
#   make aarch64-fp-smoke  がまとめてやる
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
PROBE=out/aarch64-fp-probe.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-fp-smoke')" >&2; exit 1; }
[ -f "$PROBE" ]  || { echo "missing $PROBE ('make aarch64-fp-probe')" >&2; exit 1; }

# **通常のスモークとは別のディスクを使う。** out/rootfs-*.img には触らない
TEST_DISK=out/aarch64-fp-disk.img
TEST_FSDIR=out/aarch64-fp-fs
LOG=LOGs/aarch64-fp-serial.log

# probe が 200KB 書くので、通常のスモーク (4MB) より広く取る
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
    cp "$PROBE" "$TEST_FSDIR/bin/fp-probe"
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

echo "--- AArch64 FP Serial Output ---"
cat "$LOG"
echo "----------------------------------"

# **打ち切りを黙って見逃さない。** マーカーが出ないまま止めた場合、
# 以降の判定は「中身が違う」ではなく「途中で切れた」で落ちる
if ! grep -aq "bootstrap user exit" "$LOG" 2>/dev/null; then
    echo "*** 実行が終わる前に打ち切られた ($LOG)" >&2
    echo "*** 最後の 5 行:" >&2
    tail -5 "$LOG" >&2
    exit 1
fi

# ---- 落ちた syscall を実測で拾う ------------------------------------------
#
# **「動いた」の裏で ENOSYS が出ていないことを見る。** musl は失敗した
# syscall を黙って迂回することがあり (getrandom → /dev/urandom など)、
# probe が最後まで通っても未実装が残る。カーネルは番号を 1 回だけ出すので、
# 出ていたら一覧にして落とす。P3 以降で足すものはここに出てくる
if grep -aq "ENOSYS: syscall" "$LOG"; then
    echo "*** 未実装の syscall が呼ばれた:" >&2
    grep -a "ENOSYS: syscall" "$LOG" | sed 's/^/***   /' >&2
    exit 1
fi

# **判定は CR を除いたコピーに当てる。**
#
# カーネルは termios の ONLCR に従って LF を CRLF で出す (実機のシリアル端末は
# LF だけでは行頭に戻らないため。日報2026-08-15 §12)。そのままだと
# ユーザープロセスの出力が "FP-START\r" になり、`^FP-START$` のような
# 行末アンカーが当たらない。**表示は元のログ、判定はこちら**。
#
# **対話シェルで grep を試して確かめないこと。** 環境によっては grep が
# 別実装 (ugrep など) に置き換わっていて CR を無視して一致し、
# 「手で試すと通るのにテストは落ちる」になる
tr -d '\r' < "$LOG" > "$LOG.nocr"
LOG="$LOG.nocr"

echo "--- P3-3 の判定 (FP/SIMD の退避) ---"
must_not "aarch64-fs-BAD" "$LOG"
must_not "aarch64-user-BAD" "$LOG"
grep -aq "exec      : /bin/fp-probe" "$LOG"
must_not "aarch64-init-BAD" "$LOG" "task_execve が失敗した"

grep -aqE "^FP-START$" "$LOG"
# **本命。** 親子が別の値を d8/d9 に置いたまま走り続け、切り替えを跨いでも
# 互いの値が見えないこと。保存していなければ、fork も write も成功したまま
# 値だけが入れ替わる ("落ちないバグ")
grep -aqE "^FP-PARENT-OK$" "$LOG"
grep -aqE "^FP-CHILD-OK$" "$LOG"
must_not "FP-PARENT-CLOBBERED" "$LOG" "親の FP に子の値が現れた (switch.S が退避していない)"
must_not "FP-CHILD-CLOBBERED" "$LOG" "子の FP に親の値が現れた (switch.S が退避していない)"
must_not "FP-CHILD-BAD" "$LOG"
grep -aqE "^FP-DONE$" "$LOG"
grep -aq "bootstrap user exit" "$LOG"

must_not "aarch64-exception-BAD" "$LOG"
must_not "xv6bio: disk" "$LOG"
must_not "vm: clone: 想定外のブロック写像" "$LOG"
must_not "task: fork の子に current がいない" "$LOG"

echo "aarch64 fp smoke test: PASS"
