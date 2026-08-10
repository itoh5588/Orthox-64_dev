#!/bin/bash
# P2: musl 静的リンクプログラムを aarch64 の EL0 で完走させる。
#
# ディスクの /bin/musl-probe を task_execve で読んで走らせる (P1 と同じ方針で
# カーネルには埋め込まない)。riscv64 の riscv64_musl_smoke.sh に相当するが、
# **fork / clone は見ない** — aarch64 はまだ実装していないので P3 の領分。
#
# 実行前に probe と init パスを差し替えたカーネルが要る:
#   make aarch64-musl-smoke  がまとめてやる
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
PROBE=out/aarch64-musl-probe.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-musl-smoke')" >&2; exit 1; }
[ -f "$PROBE" ]  || { echo "missing $PROBE ('make aarch64-musl-probe')" >&2; exit 1; }

# **通常のスモークとは別のディスクを使う。** out/rootfs-*.img には触らない
TEST_DISK=out/aarch64-musl-disk.img
TEST_FSDIR=out/aarch64-musl-fs
LOG=LOGs/aarch64-musl-serial.log

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
    cp "$PROBE" "$TEST_FSDIR/bin/musl-probe"
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

echo "--- AArch64 musl Serial Output ---"
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

echo "--- P2 の判定 ---"
# 起動時の自己診断が緑であること。**ここが赤いまま P2 の判定に進むと、
# probe の失敗とディスクの不備が見分けられなくなる**
must_not "aarch64-fs-BAD" "$LOG"
must_not "aarch64-user-BAD" "$LOG"
# exec が成立していること。ここで落ちるなら ELF の読み込み経路
grep -aq "exec      : /bin/musl-probe" "$LOG"
must_not "aarch64-init-BAD" "$LOG" "task_execve が失敗した (ELF が読めていない)"

# **libc を通した最初の syscall。** ここが通れば crt0 と
# __libc_start_main の引数の積み方が正しい
grep -aq "MUSL:/" "$LOG"
# 自分自身を開いて ELF マジックまで照合できた = fd 層 + xv6fs の読み
grep -aqE "^ELF$" "$LOG"
# 匿名 mmap に書いて読み返せた
grep -aqE "^MAP$" "$LOG"
# **ディスクへの新規作成 (O_CREAT) が返ってくること。**
#
# ここは調査用に足した段階マーカーをそのまま検査に残したもの。
# svc 処理中に IRQ がマスクされていると、**例外も出さずに vblk_rw の
# 完了待ちループで永久に止まる** (gdbstub で PC=vblk_rw+288 /
# PSTATE.I=1 を実測)。open(O_RDONLY) は書き込みを伴わないので通ってしまい、
# 段階を分けていないと「どの open で止まったか」が読めない
grep -aqE "^BW-ROOTOPEN$" "$LOG"     # root 直下への O_CREAT
grep -aqE "^BW-OPEN$" "$LOG"         # サブディレクトリ (/tmp) への O_CREAT
grep -aqE "^BW-4K$" "$LOG"           # ログ容量に収まる write
# xv6fs のログ容量を超える 1 回の write が分割されている
# (無いと KASSERT でカーネルパニックする)
grep -aqE "^BIGWRITE-OK$" "$LOG"
# **「書ける FS が無くて静かに飛ばした」を PASS にしない。**
# BIGWRITE / DUPRW は open に失敗すると丸ごと飛ぶ作りなので、
# ここを見ないとディスクを繋ぎ忘れた回も緑になる
must_not "BW-NOOPEN" "$LOG" "/tmp への O_CREAT が失敗した (検査が丸ごと飛んでいる)"
# dup した fd から読み戻せる (無いと ar が 0 バイトの .a を作る)
grep -aqE "^DUPRW-OK$" "$LOG"
grep -aqE "^DONE$" "$LOG"
grep -aq "bootstrap user exit" "$LOG"

# 想定外の例外を踏んでいないこと
must_not "aarch64-exception-BAD" "$LOG"
must_not "xv6bio: disk" "$LOG"

echo "aarch64 musl smoke test: PASS"
