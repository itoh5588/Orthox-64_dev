#!/bin/bash
# P3-4: fork した子のアドレス空間が返っているかの検査。
#
# ディスクの /bin/vmleak-probe を task_execve で読んで走らせる (P1 と同じ方針で
# カーネルには埋め込まない)。riscv64 の riscv64_musl_smoke.sh に相当するが、
# **fork / clone は見ない** — aarch64 はまだ実装していないので P3 の領分。
#
# 実行前に probe と init パスを差し替えたカーネルが要る:
#   make aarch64-vmleak-smoke  がまとめてやる
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
PROBE=out/aarch64-vmleak-probe.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-vmleak-smoke')" >&2; exit 1; }
[ -f "$PROBE" ]  || { echo "missing $PROBE ('make aarch64-vmleak-probe')" >&2; exit 1; }

# **通常のスモークとは別のディスクを使う。** out/rootfs-*.img には触らない
TEST_DISK=out/aarch64-vmleak-disk.img
TEST_FSDIR=out/aarch64-vmleak-fs
LOG=LOGs/aarch64-vmleak-serial.log

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
    cp "$PROBE" "$TEST_FSDIR/bin/vmleak-probe"
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

echo "--- AArch64 vmleak Serial Output ---"
cat "$LOG"
echo "----------------------------------"

# **判定は CR を除いたコピーに当てる。**
#
# カーネルは termios の ONLCR に従って LF を CRLF で出す (実機のシリアル端末は
# LF だけでは行頭に戻らないため。日報2026-08-15 §12)。そのままだと
# ユーザープロセスの出力が "VMLEAK-START\r" になり、`^VMLEAK-START$` の
# ような行末アンカーが当たらない。**表示は元のログ、判定はこちら**。
#
# **対話シェルで grep を試して確かめないこと。** 環境によっては grep が
# 別実装 (ugrep など) に置き換わっていて CR を無視して一致し、
# 「手で試すと通るのにテストは落ちる」になる
tr -d '\r' < "$LOG" > "$LOG.nocr"
LOG="$LOG.nocr"

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

echo "--- P3-4 の判定 (アドレス空間の解放) ---"
must_not "aarch64-fs-BAD" "$LOG"
must_not "aarch64-user-BAD" "$LOG"
grep -aq "exec      : /bin/vmleak-probe" "$LOG"
must_not "aarch64-init-BAD" "$LOG" "task_execve が失敗した"

grep -aqE "^VMLEAK-START$" "$LOG"
# 大量にまわしても壊れないこと。**ただしここは漏れを捕まえられない** —
# 修正前の状態でも 1600 回は全部成功する (実測。尽きるのは 1736 回目あたり)。
# 漏れの判定は下の空きページ数のほう
grep -aqE "^VMLEAK-ROUNDS:1600$" "$LOG"
grep -aqE "^VMLEAK-OK$" "$LOG"
must_not "VMLEAK-FORK-FAILED-AT:" "$LOG" "fork が途中で失敗した = 子の空間が返っていない"
grep -aq "bootstrap user exit" "$LOG"

# ---- ★ 漏れを捕まえるのはここ ------------------------------------------
#
# カーネルが arch_halt_forever で出す実測値。**逆確認で両方の値を取った**:
#
#   返している    130099 / 130264  (1600 回まわして 151 ページしか減らない)
#   返していない   10097 / 130264  (1 回あたり 75 ページ漏れる)
#
# 桁が違うので、しきい値は半分で十分
grep -aq "  pmm 残り  : " "$LOG"
free_pages=$(grep -a "  pmm 残り  : " "$LOG" | head -1 | sed 's/.*: 0x\([0-9a-f]*\) .*/\1/')
free_dec=$((16#$free_pages))
echo "  fork/exit を 1600 回まわした後の空き: $free_dec ページ"
# 起動直後が 0x1fcca (130250) ページ。漏れていなければ大半が残っているはず。
# **半分を切っていたら、返し切れていない**
if [ "$free_dec" -lt 65000 ]; then
    echo "*** 空きページが半分を切っている ($free_dec)。空間を返し切れていない" >&2
    exit 1
fi

must_not "aarch64-exception-BAD" "$LOG"
must_not "vm: destroy: 想定外のブロック写像" "$LOG"
must_not "vm: clone: 想定外のブロック写像" "$LOG"

echo "aarch64 vmleak smoke test: PASS"
