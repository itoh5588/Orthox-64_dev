#!/bin/bash
# Orthox 上で cc1 の .o を作りきる (実測で 20 時間前後かかる)。
#
#   scripts/run_riscv64_gcc_full.sh
#
# 1 回の QEMU 起動で作れるところまで作り、止まったら殺して起動し直す。
# 飛ばす判定は完成印 (.ok) なので、何度再起動しても進んだ分は失われず、
# 書きかけの .o を掴むこともない。
#
# 「止まった」の判定は QEMU の CPU 時間で行う。大きいファイルは 40 分以上
# 無出力のまま計算し続けるので、ログの無進捗では判定できない。
#
# 環境変数で上書きできる:
#   IMG KERNEL BIOS LOGDIR LIMIT MAXROUND STALL MEM
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${IMG:-$ROOT/out/rootfs-riscv64-xv6.img}
KERNEL=${KERNEL:-$ROOT/out/kernel-riscv64.elf}
BIOS=${BIOS:-/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin}
LOGDIR=${LOGDIR:-$ROOT/logs/gccfull}
LIMIT=${LIMIT:-0}          # 0 = objlist を最後まで
MAXROUND=${MAXROUND:-40}
STALL=${STALL:-120}        # CPU が止まって何秒でハングとみなすか
MEM=${MEM:-2048M}

mkdir -p "$LOGDIR"
SUMMARY=$LOGDIR/summary.log

say() { echo "[$(date '+%m-%d %H:%M:%S')] $*" | tee -a "$SUMMARY"; }
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
built() { grep -ac "^CC " "$1" 2>/dev/null || echo 0; }

for f in "$IMG" "$KERNEL" "$BIOS"; do
    [ -f "$f" ] || { echo "見つからない: $f" >&2; exit 1; }
done

say "開始  img=$IMG"
say "      kernel=$KERNEL  stall=${STALL}s  最大 $MAXROUND ラウンド"

round=0
zero=0
total=0
finished=0

while [ "$round" -lt "$MAXROUND" ]; do
    round=$((round + 1))
    LOG=$LOGDIR/round$(printf '%02d' "$round").log
    FIFO=$LOGDIR/.in.$$

    rm -f "$FIFO"
    mkfifo "$FIFO" || exit 1

    # 投入側。QEMU を殺しても残らないよう pid を控えて後で始末する
    {
        sleep 10
        printf '%s\n' 'export PATH=/bin:/usr/bin'
        sleep 2
        printf '%s\n' "cd /src/gcc-full/build/gcc && sh build_cc1.sh $LIMIT"
        while true; do sleep 60; done
    } > "$FIFO" &
    FEEDER=$!

    qemu-system-riscv64 -machine virt -cpu rv64 -m "$MEM" -smp 1 \
        -bios "$BIOS" -kernel "$KERNEL" -display none -serial stdio -monitor none \
        -drive file="$IMG",if=none,format=raw,id=vblk0 \
        -device virtio-blk-device,drive=vblk0 < "$FIFO" > "$LOG" 2>&1 &
    QPID=$!

    say "ラウンド $round 開始 (qemu pid=$QPID) → $LOG"

    reason=""
    while kill -0 "$QPID" 2>/dev/null; do
        if grep -aq "CC1BUILD-DONE" "$LOG" 2>/dev/null; then
            reason="完了"
            finished=1
            break
        fi
        a=$(cpu_ticks "$QPID")
        sleep "$STALL"
        b=$(cpu_ticks "$QPID")
        if [ $((b - a)) -lt 20 ]; then
            reason="停止を検出 (${STALL}秒で CPU tick $((b - a)))"
            break
        fi
    done
    [ -n "$reason" ] || reason="QEMU が自分で終了"

    kill "$QPID" 2>/dev/null
    kill "$FEEDER" 2>/dev/null
    wait "$QPID" 2>/dev/null
    wait "$FEEDER" 2>/dev/null
    rm -f "$FIFO"

    n=$(built "$LOG")
    total=$((total + n))
    say "ラウンド $round 終了: $reason / このラウンド $n 本 (累計 $total 本)"

    if [ "$finished" = 1 ]; then
        say "全部できた。ラウンド数 $round、累計 $total 本"
        grep -a "CC1BUILD-DONE" "$LOG" | tail -1 | sed 's/^/          /' | tee -a "$SUMMARY"
        exit 0
    fi

    if [ "$n" -eq 0 ]; then
        zero=$((zero + 1))
        if [ "$zero" -ge 3 ]; then
            say "3 ラウンド続けて 1 本も進まない。何か別の原因があるので中断する"
            say "最後のログの末尾:"
            tail -c 500 "$LOG" | sed 's/^/          /' | tee -a "$SUMMARY"
            exit 1
        fi
        say "  (このラウンドは 1 本も進んでいない。連続 $zero 回目)"
    else
        zero=0
    fi
done

say "最大ラウンド数 $MAXROUND に達した。累計 $total 本。まだ終わっていない"
exit 1
