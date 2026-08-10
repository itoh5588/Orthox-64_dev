#!/bin/bash
# P3-2: コンソール入力 (PL011 の受信割り込み) の検査。
#
# 他のスモークと違い **QEMU の標準入力に文字を流し込む。**
# probe が CONSOLE-READY を出す = read(0) で寝る直前まで来た、という合図。
# それを見てから送ることで、**「寝ている所へ割り込みで届く」経路**を通す。
# 先に送ってしまうと、リングに溜まったものを 1 回目の kb_read が拾えてしまい、
# 割り込みが死んでいても通る検査になる。
#
#   make aarch64-console-smoke がビルドとあわせて実行する
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
PROBE=out/aarch64-console-probe.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-console-smoke')" >&2; exit 1; }
[ -f "$PROBE" ]  || { echo "missing $PROBE ('make aarch64-console-probe')" >&2; exit 1; }

TEST_DISK=out/aarch64-console-disk.img
TEST_FSDIR=out/aarch64-console-fs
LOG=LOGs/aarch64-console-serial.log
XV6FS_TEST_BLOCKS=4096

must_not() {   # $1 = 固定文字列, $2 = ログ, $3 = 説明 (任意)
    if grep -aqF -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た: $1  ($2)" >&2
        [ -n "${3:-}" ] && echo "*** $3" >&2
        grep -aF -- "$1" "$2" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

mkdir -p out
rm -rf "$TEST_FSDIR"
mkdir -p "$TEST_FSDIR/bin"
cp "$PROBE" "$TEST_FSDIR/bin/console-probe"
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
rm -f "$TEST_DISK"
XV6FS_FSSIZE=$XV6FS_TEST_BLOCKS XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null

rm -f "$LOG"

# **QEMU は必ずこちらから止める。**
#
# カーネルは bootstrap user exit のあと aarch64_wait_forever で回り続けるので、
# QEMU は自分から終わらない。`( ... ) | qemu` の形で繋ぐと待ち合わせる相手が
# 居なくなり、**スモークが永久に返らない** (実際に 18 分回り続けた)。
# 他のスモークと同じく、バックグラウンドで起動してマーカーを見て kill する。
#
# 入力は名前付きパイプ経由で送る。パイプを閉じないので EOF にもならない。
FIFO="$(mktemp -u)"
mkfifo "$FIFO"

QEMU_PID=""
FEEDER_PID=""
cleanup() {
    [ -n "$FEEDER_PID" ] && kill "$FEEDER_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$FIFO"
}
trap cleanup EXIT

"$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp 1 \
    -nographic \
    -drive "file=$TEST_DISK,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 \
    -kernel "$KERNEL" < "$FIFO" > "$LOG" 2>&1 &
QEMU_PID=$!

# **書き込み側を開いたままにする。** 閉じると QEMU の stdin が EOF になる
exec 9>"$FIFO"

# **CONSOLE-READY を待ってから送る。** 固定の sleep にすると、起動が遅れた回に
# 「まだ read に入っていない」所へ送ってしまい、割り込みを検証できない
ready=0
for _ in $(seq 1 60); do
    if grep -aq "CONSOLE-READY" "$LOG" 2>/dev/null; then ready=1; break; fi
    sleep 1
done
if [ "$ready" = 1 ]; then
    sleep 2                      # probe が read で寝るまでの余裕
    printf 'hello-stdin\n' >&9
fi

# 出力が出そろうのを待つ。**マーカーを見る** (固定の sleep にしない)
for _ in $(seq 1 20); do
    if grep -aq "bootstrap user exit" "$LOG" 2>/dev/null; then break; fi
    sleep 1
done
exec 9>&-
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- AArch64 console Serial Output ---"
cat "$LOG"
echo "-------------------------------------"

if ! grep -aq "CONSOLE-READY" "$LOG" 2>/dev/null; then
    echo "*** probe が read まで到達していない ($LOG)" >&2
    tail -5 "$LOG" >&2
    exit 1
fi

if grep -aq "ENOSYS: syscall" "$LOG"; then
    echo "*** 未実装の syscall が呼ばれた:" >&2
    grep -a "ENOSYS: syscall" "$LOG" | sed 's/^/***   /' >&2
    exit 1
fi

echo "--- P3-2 の判定 (コンソール入力) ---"
must_not "aarch64-fs-BAD" "$LOG"
must_not "aarch64-user-BAD" "$LOG"
grep -aq "exec      : /bin/console-probe" "$LOG"
must_not "aarch64-init-BAD" "$LOG" "task_execve が失敗した"

# **UART の割り込み番号を DTB から取れていること。**
# 既定値に落ちていても QEMU virt では同じ 33 になるので、
# ここを見ないと「DTB を読めていない」ことに気づけない
grep -aq "uart irq  : 0x0000000000000021  (dtb)" "$LOG"

# 寝ている所へ割り込みで文字が届いた
grep -aqE "^CONSOLE-GOT:hello-stdin$" "$LOG"
# 中身まで一致した = リングの読み書きがずれていない
grep -aqE "^CONSOLE-MATCH$" "$LOG"
must_not "CONSOLE-MISMATCH" "$LOG" "読めたが中身が違う (リングの head/tail がずれている)"
grep -aqE "^CONSOLE-DONE$" "$LOG"
grep -aq "bootstrap user exit" "$LOG"

must_not "aarch64-exception-BAD" "$LOG"

echo "aarch64 console smoke test: PASS"
