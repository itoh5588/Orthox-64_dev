#!/bin/bash
# x86_64 カーネルの回帰テスト (SMP + スケジューラ/タイマー周り)。
#
# ねらい: riscv64 の作業で kernel/sched.c と kernel/task.c を触っているが、
# これらは x86 と共有している。macOS では x86 ユーザーランドをビルドできない
# ものの、**既存の rootfs.img に必要なテストプログラムが入っている**ので、
# 新しくビルドしたカーネルと組み合わせれば実行検証はできる。
#
# 起床レイテンシについて:
# riscv64 で入れた「起床時に resched を要求する」修正 (task_poll_sleep_wakeups)
# は x86 でも効く。以前ここには「x86 では実測経過が変わらなかった」と書いて
# あったが、それは 1 回しか測っていなかったための誤りだった。
#
#   resched 要求あり  sleep_ms(120) -> 120 120 120 120 120 120
#   resched 要求なし  sleep_ms(120) -> 130 140 140 160 150 170
#
# 差が riscv64 (2601ms -> 1033ms) より小さいのは、x86 には resched のもう一つの
# 出どころがあるため。task_on_timer_tick() は idle タスクも「RUNNING の
# タスク」として扱うので、そのタイムスライスが尽きるたびに resched を要求する
# = TASK_TIMESLICE_TICKS(5) x SCHED_TICK_MS(10ms) = 50ms ごと。
# よって起床時の要求が無くても、待たされるのは最大 50ms (10ms 刻み) で済む。
# riscv64 は tick が 100ms なので同じ構造で 500ms になる。
# sleep_ms(250) がほぼ常に 250ms なのは、250 が 50ms の倍数で、起床する tick が
# idle の resched tick と重なりやすいから。ここを 1 回だけ測ると「効果が無い」
# ように見える。**下ではそれを踏まないよう 4 回測って全サンプルを見ている。**
#
# ここで守れるのはそれに加えて「タイマー割り込み文脈から IPI を飛ばすように
# なっても SMP でハング/クラッシュしない」ことと、スケジューラが壊れていないこと。
#
# 使うもの (すべて rootfs.img に既存):
#   /bin/testtime.elf      sleep_ms の実測経過 -> 起床レイテンシ
#   /bin/tickratecheck.elf 1 秒 x 5 回の sleep -> タイマー起床の連続性
#   /bin/smpstress.elf     fork/exec を回して複数 CPU に散らす -> resched IPI
#
#   make x86-kernel-smoke がカーネルのビルドとあわせて実行する
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

SMP_CPUS="${SMP_CPUS:-4}"
SERIAL_LOG=LOGs/x86-kernel-serial.log
WORK="$(mktemp -d)"
ISO="$WORK/x86-kernel-smoke.iso"
rm -f "$SERIAL_LOG"

cleanup() {
    kill "${QEMU_PID:-0}" 2>/dev/null || true
    wait "${QEMU_PID:-0}" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

# ISO を組む。make の $(ISO) ルールと同じ手順だが、rootfs.img は
# 既存のものをそのまま使う (macOS ではユーザーランドを再ビルドできないため)
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

# ブート直後に /etc/bootcmd のスモークが走るので、それが終わるまで待ってから流す
(
    sleep 25
    # 4 回測る。遅れ幅は 0..50ms に散るので、1 回だけだと遅れ 0 のサンプルを
    # 引いて見逃す (下で「全サンプル」を見るのはこのため)
    printf '/bin/testtime.elf\n'
    sleep 8
    printf '/bin/testtime.elf\n'
    sleep 8
    printf '/bin/testtime.elf\n'
    sleep 8
    printf '/bin/testtime.elf\n'
    sleep 8
    printf '/bin/tickratecheck.elf\n'
    sleep 12
    printf '/bin/smpstress.elf 2\n'
    sleep 25
    printf 'echo x86-smoke-done\n'
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

for _ in {1..120}; do
    if grep -aq "x86-smoke-done" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- x86 kernel Serial Output ---"
cat "$SERIAL_LOG"
echo "--------------------------------"

# シェルは 1 文字ずつエコーバックするので、grep はコマンド名ではなく
# 出力側にしか現れない文字列に当てること
grep -aq "Task system initialized" "$SERIAL_LOG"
# ブート時の bootcmd (OS 内でカーネルを組む)。
# ここだけが落ちるときはカーネルの退行ではなく、rootfs.img の /kbuild が
# 空でフルビルドになり持ち時間に間に合っていない可能性が高い。
# その場合は make nativekernelbuildsmoke を 1 回流してから出直すこと
# (日報2026-08-08 §5、Makefile の x86-*-smoke 手前のコメントも参照)。
grep -aq "kernel-native-build: PASS" "$SERIAL_LOG"

# 起床レイテンシ: sleep_ms(120) と sleep_ms(250) の実測
grep -aq "tick0=" "$SERIAL_LOG"
grep -aq "tick1=" "$SERIAL_LOG"
grep -aq "tick2=" "$SERIAL_LOG"
python3 - "$SERIAL_LOG" <<'PY'
import re, sys
log = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
deltas = [int(m) for m in re.findall(r'\(delta=(\d+)\)', log)]
# testtime.elf は 1 回の実行で (120, 250) の順に 2 個出す
runs = list(zip(deltas[0::2], deltas[1::2]))
if not runs:
    print("testtime: delta が取れない: %r" % deltas); sys.exit(1)
print("testtime: %s" % ", ".join("120->%dms/250->%dms" % r for r in runs))

for d120, d250 in runs:
    # 寝ていない (短すぎる) / 起床が極端に遅い のどちらも落とす
    if not (100 <= d120 <= 400):
        print("testtime: sleep_ms(120) の実測が範囲外"); sys.exit(1)
    if not (230 <= d250 <= 600):
        print("testtime: sleep_ms(250) の実測が範囲外"); sys.exit(1)

# 起床時に resched を要求しているか。要求が無いと idle のタイムスライスが
# 尽きるまで待たされる (0..50ms のどこか)。
#
# 判定は「全サンプルが 130ms 以下」。
#   - 上限が 130 なのは、tick が 10ms なので寝始めた位置によって 1 tick 分の
#     量子化が乗るため (SMP=1 で実測 130ms)。これはスケジューラの遅れではない
#   - 最小値で見てはいけない: 要求が無くてもたまたま遅れ 0 のサンプルが混じる
#     ので、最小値だと素通りする (実際に 3 回の最小値で試して見逃した)
#   - 要求が無いときは 130..170ms に散るので、4 サンプルのどれかが 130 を超える。
#     全部が 130 以下に収まる確率は数 % 残るが、それ以上詰めると量子化と
#     区別できなくなる
worst = max(d for d, _ in runs)
print("testtime: sleep_ms(120) の最大 = %dms (%d サンプル)" % (worst, len(runs)))
if worst > 130:
    print("testtime: 起床が遅い。task_poll_sleep_wakeups() の resched 要求を疑う")
    sys.exit(1)
PY

# タイマー起床が 5 回続けて効く
[ "$(grep -ac "tickratecheck step=" "$SERIAL_LOG")" = "5" ]

# fork/exec を回して複数 CPU に散らす負荷試験
grep -aq "smpstress: PASS" "$SERIAL_LOG"

grep -aq "x86-smoke-done" "$SERIAL_LOG"

echo "x86 kernel smoke test: PASS (smp=$SMP_CPUS)"
