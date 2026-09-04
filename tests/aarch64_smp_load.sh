#!/bin/bash
# SMP の負荷試験 (P-7)。**複数コアで ash を動かし、fork を続けて回す。**
#
# tests/aarch64_ash_smoke.sh が「1 コアで一通り動く」を見るのに対し、
# こちらは **複数コアでしか出ない取りこぼし** を狙う。既定は 4 コア。
#
# ---- なぜこの並びなのか --------------------------------------------------
#
# **本命は `$(cmd | cmd)`。** riscv64 で 2026-08-05 に踏んだもので、原因は
# 「子が親を起こしていない」だった。**単一 CPU では露見しない wakeup の
# 取りこぼしが、SMP で初めて出る類**なので、最初に置く。
#
# 次に「fork を短い間隔で繰り返す」。タスクが CPU をまたいで置かれるので、
# **CPU ごとのレジスタの取りこぼし**があるとここで出る。P-5 では
# CPACR_EL1 (FP/NEON) と GIC の PPI (タイマ) の 2 つが出た。
#
# SMP_CPUS=1 と比べると「SMP のせいか」が切り分けられる。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
[ -n "$QEMU_BIN" ] || { echo "qemu-system-aarch64 not found" >&2; exit 1; }

KERNEL=out/kernel-aarch64.elf
ASH=out/busybox-aarch64-musl.elf
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-smp-load')" >&2; exit 1; }
[ -f "$ASH" ]    || { echo "missing $ASH ('make aarch64-busybox-musl')" >&2; exit 1; }

TEST_DISK=out/aarch64-smp-disk.img
TEST_FSDIR=out/aarch64-smp-fs
LOG=LOGs/aarch64-smp-serial.log
CPUS="${SMP_CPUS:-4}"

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

must_not() {
    if grep -aqF -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た: $1  ($2)" >&2
        grep -aF -- "$1" "$2" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

mkdir -p out
rm -rf "$TEST_FSDIR"
mkdir -p "$TEST_FSDIR/bin" "$TEST_FSDIR/tmp" "$TEST_FSDIR/etc"
for applet in ash echo cat wc uname sort ls mkdir rm sleep grep true; do
    cp "$ASH" "$TEST_FSDIR/bin/$applet"
done
printf 'a\nb\nc\n' > "$TEST_FSDIR/three.txt"
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
rm -f "$TEST_DISK"
XV6FS_FSSIZE=16384 XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null

rm -f "$LOG"

(
    sleep 10
    printf 'echo smp-load-start\n'
    sleep 3
    # ★ 本命。コマンド置換の中のパイプ (riscv64 で 2026-08-05 に止まった形)
    printf 'echo subst=$(cat /three.txt | wc -l)\n'
    sleep 5
    # 2 段のパイプ = fork 3 つ
    printf 'cat /three.txt | sort | wc -l\n'
    sleep 5
    # 外部 exec を続けて 8 回。CPU をまたいで置かれる
    printf 'i=0; while [ $i -lt 8 ]; do /bin/echo spin-$i; i=$((i+1)); done\n'
    sleep 12
    # 入れ子のコマンド置換
    printf 'echo nest=$(echo $(cat /three.txt | wc -l))\n'
    sleep 5
    # 背景ジョブを 3 本まとめて
    printf '/bin/echo bg1 & /bin/echo bg2 & /bin/echo bg3 &\n'
    sleep 6
    printf 'echo after-bg\n'
    sleep 3
    # 寝て起きる (タイマがコアごとに効いているか)
    printf 'sleep 1; echo slept\n'
    sleep 5
    printf 'echo smp-load-done\n'
    sleep 3
    printf 'exit\n'
    sleep 3
) | "$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp "$CPUS" \
    -display none \
    -serial stdio \
    -monitor none \
    -drive "file=$TEST_DISK,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 \
    -kernel "$KERNEL" > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..150}; do
    grep -aq "bootstrap user exit" "$LOG" 2>/dev/null && break
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- AArch64 SMP Load Serial Output (cpus=$CPUS) ---"
cat "$LOG"
echo "---------------------------------------------------"
echo "--- P-7 の判定 (SMP 負荷) ---"

tr -d '\r' < "$LOG" > "$LOG.nocr"
LOG="$LOG.nocr"

grep -aq "bootstrap user exit" "$LOG" || {
    echo "*** 'bootstrap user exit' が出ないまま打ち切った (どこかで止まった)" >&2
    exit 1
}

must_not "PANIC"                  "$LOG"
must_not "aarch64-exception-BAD"  "$LOG" 
must_not "aarch64-smp-BAD"        "$LOG"
must_not "aarch64-init-BAD"       "$LOG"

# **副コアが実際に上がっていること。** 1 コアで走らせたときは飛ばす
if [ "$CPUS" != "1" ]; then
    grep -aq "aarch64-smp-ok" "$LOG"
    want=$((CPUS - 1))
    got="$(grep -ac "joined shared scheduler" "$LOG" || true)"
    [ "$got" = "$want" ] || {
        echo "*** 副コアが $want 本のはずが $got 本しか上がっていない" >&2
        exit 1
    }
    # IPI が全部届いていること
    [ "$(grep -ac "SGI was received" "$LOG")" = "$want" ]
fi

grep -aq "smp-load-start" "$LOG"
grep -aq "subst=3"        "$LOG"          # ★ コマンド置換の中のパイプ
grep -aqE "^ *3$"         "$LOG"          # 2 段パイプ
for i in 0 1 2 3 4 5 6 7; do
    grep -aq "spin-$i" "$LOG" || { echo "*** spin-$i が出ていない" >&2; exit 1; }
done
grep -aq "nest=3"         "$LOG"          # 入れ子のコマンド置換
# **行頭アンカーで数えない。** 背景ジョブの出力とプロンプトは非同期なので、
# 実測で `# bg2` のようにプロンプトが行頭に付く。これは表示の綾であって
# 異常ではない (bg1/bg3 は素で出ていた)。
#
# **数えるのは出現回数。** コマンドのエコー行に 1 回 + 出力に 1 回 = 2 回。
# 子が 2 度走れば 3 回になるので、「1 度だけ走った」の判定はこれで足りる
for b in bg1 bg2 bg3; do
    n="$(grep -ac "$b" "$LOG")"
    [ "$n" = "2" ] || {
        echo "*** $b の出現が $n 回 (エコー 1 + 出力 1 = 2 のはず)" >&2; exit 1; }
done
grep -aq "after-bg"       "$LOG"
grep -aq "slept"          "$LOG"
grep -aq "smp-load-done"  "$LOG"

echo "aarch64 SMP load test (cpus=$CPUS): PASS"
