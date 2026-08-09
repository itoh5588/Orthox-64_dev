#!/bin/bash
# AArch64 (QEMU virt) の起動スモーク。M0: PL011 に文字が出るところまで。
#
# 日報2026-08-02 の段取り 1 の最初の一歩。ツールチェーン (clang の aarch64
# ターゲット) / リンカスクリプト / QEMU の起動 / シリアル経路を一度に見る。
#
# 判定は「動いた」だけにしない。EL / MPIDR / DTB / bss ゼロ埋めを表示させ、
# 中身が妥当かまで見る (起動できても bss が埋まっていなければ、この先の C が
# 静かに壊れる)。
#
#   make aarch64-smoke がビルドとあわせて実行する
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
[ -f "$KERNEL" ] || { echo "missing $KERNEL ('make aarch64-kernel')" >&2; exit 1; }

QEMU_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 2 通りで回す。
#   virt                    既定。EL1 から始まる
#   virt,virtualization=on  **EL2 から始まる**。Raspberry Pi 4 の
#                           ファームウェアと同じ条件を QEMU で再現できる
#
# 降格を入れる前は後者で ticks が 0 になった (EL2 では VBAR_EL1 の表が
# 使われないため)。実機が無くても EL2 経路を検証できる。
run_one() {  # $1 = -machine の値、$2 = ログ
    local machine="$1" log="$2"
    rm -f "$log"
    "$QEMU_BIN" \
        -machine "$machine" \
        -cpu cortex-a72 \
        -m 512M \
        -smp 1 \
        -nographic \
        -kernel "$KERNEL" < /dev/null > "$log" 2>&1 &
    QEMU_PID=$!
    for _ in {1..30}; do
        if grep -aqE "aarch64-timer-(ok|BAD)" "$log" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
}

check_one() {  # $1 = 見出し、$2 = ログ
    echo "--- AArch64 Serial Output ($1) ---"
    cat "$2"
    echo "----------------------------------"

    # M0: 起動して PL011 に出る
    grep -aq "Orthox-64 aarch64 boot" "$2"
    # EL2 で始まっても降格して EL1 に居ること。EL2 のままだと tick を取りこぼす
    grep -aq "CurrentEL : EL1" "$2"
    grep -aq "bss zero  : ok" "$2"        # start.S のゼロ埋めが効いていること
    ! grep -aq "bss zero  : BAD" "$2"
    grep -aq "aarch64-boot-ok" "$2"

    # M1: 例外ベクタ + GIC + generic timer で tick が入る
    #
    # 逆確認済み (日報2026-08-08):
    #   daifclr を外す        -> ticks 0 で timer-BAD を出して進む
    #   GIC で有効化しない    -> timer freq の次で止まり ticks 行すら出ない
    # 別々の壊れ方をするので、まとめて外さずに 1 つずつ確かめてある
    grep -aq "timer freq: 0x0000000003b9aca0" "$2"   # 62.5MHz (QEMU virt の実測値)
    grep -aq "aarch64-timer-ok" "$2"
    ! grep -aq "aarch64-timer-BAD" "$2"

    # 想定外の例外を踏んでいないこと
    ! grep -aq "aarch64-exception-BAD" "$2"
}

run_one "virt" LOGs/aarch64-serial.log
check_one "EL1 起動" LOGs/aarch64-serial.log

run_one "virt,virtualization=on" LOGs/aarch64-serial-el2.log
check_one "EL2 起動 -> 降格" LOGs/aarch64-serial-el2.log

echo "aarch64 smoke test: PASS (M0 + M1, EL1 起動と EL2 降格の 2 通り)"
