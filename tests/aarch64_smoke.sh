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

# 3 通りで回す。
#   virt                    既定。EL1 から始まる
#   virt,virtualization=on  **EL2 から始まる**。Raspberry Pi 4 の
#                           ファームウェアと同じ条件を QEMU で再現できる
#   virt (-m 1G)            **DTB を本当に読んでいるかの確認。**
#                           直書きの 512MB では追随できない
#
# 降格を入れる前は 2 通り目で ticks が 0 になった (EL2 では VBAR_EL1 の表が
# 使われないため)。実機が無くても EL2 経路を検証できる。
run_one() {  # $1 = -machine の値、$2 = ログ、$3 = -m の値
    local machine="$1" log="$2" mem="${3:-512M}"
    rm -f "$log"
    "$QEMU_BIN" \
        -machine "$machine" \
        -cpu cortex-a72 \
        -m "$mem" \
        -smp 1 \
        -nographic \
        -kernel "$KERNEL" < /dev/null > "$log" 2>&1 &
    QEMU_PID=$!
    for _ in {1..30}; do
        if grep -aqE "aarch64-mmu-(ok|BAD)" "$log" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""
}

check_one() {  # $1 = 見出し、$2 = ログ、$3 = 期待する RAM 容量 (16 桁 hex)
    local ram_size="${3:-0x0000000020000000}"
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

    # M2b: DTB
    #
    # **「値が正しい」だけでは DTB を読んだ証拠にならない。** 直書きの既定値と
    # 同じ値になるので、既定値のまま進んでいても出力は同じになる。実際、
    # 深さごとの状態を持たずに書いていたとき、intc の子ノード (v2m) に
    # 状態を潰されて GIC だけ既定値のまま進んでいた。
    # そこで **どこから来た値かを (dtb) / (既定値) で出させて判定する。**
    grep -aq "aarch64-dtb-ok" "$2"
    ! grep -aq "aarch64-dtb-BAD" "$2"
    grep -aq "memory    : 0x0000000040000000 size ${ram_size}  (dtb)" "$2"
    grep -aq "uart      : 0x0000000009000000  (dtb)" "$2"
    # GIC は reg[0] と reg[1] の**両方**が取れて初めて (dtb) になる
    grep -aq "gic dist  : 0x0000000008000000 size 0x0000000000010000  (dtb)" "$2"
    grep -aq "gic cpu   : 0x0000000008010000 size 0x0000000000010000  (dtb)" "$2"
    grep -aq "virtio    : 0x000000000a000000 x 0x0000000000000020 stride 0x0000000000000200  (dtb)" "$2"
    grep -aq "timer irq : 0x000000000000001e  (dtb)" "$2"   # PPI 14 + 16 = 30
    ! grep -aq "(既定値)" "$2"     # 1 つでも既定値に落ちていたら失格

    # M1: 例外ベクタ + GIC + generic timer で tick が入る
    #
    # 逆確認済み (日報2026-08-08):
    #   daifclr を外す        -> ticks 0 で timer-BAD を出して進む
    #   GIC で有効化しない    -> timer freq の次で止まり ticks 行すら出ない
    # 別々の壊れ方をするので、まとめて外さずに 1 つずつ確かめてある
    grep -aq "timer freq: 0x0000000003b9aca0" "$2"   # 62.5MHz (QEMU virt の実測値)
    grep -aq "aarch64-timer-ok" "$2"
    ! grep -aq "aarch64-timer-BAD" "$2"

    # M2: MMU (恒等マッピングで有効化)
    #
    # **恒等マッピングだけでは「MMU が効いている」証拠にならない。** VA == PA
    # なので、MMU を入れ忘れても出力はまったく同じになる。判定は 3 本立て:
    #
    #   1. SCTLR_EL1 の M / C / I が立っていること (レジスタの実測値)
    #   2. 未マップの VA を読んで translation fault が上がること (翻訳の証拠)
    #   3. MMU on のまま tick が入り続けること (GIC を Device 属性で
    #      張れているか。ここを落とすと MMU on の瞬間に沈黙する)
    grep -aq "M2: MMU (identity, 4KB granule, VA 39bit)" "$2"
    # カーネルが自分で決めた値。**EL1 起動と EL2 降格で同じ値になること**が
    # 要点 (起動時の値を読んで OR していたときは別の値になっていた)
    grep -aq "SCTLR_EL1 : 0x0000000030d0181d" "$2"
    grep -aq "uart ->pa : 0x0000000009000000" "$2"   # 恒等に張れていること
    grep -aq "gicd ->pa : 0x0000000008000000" "$2"
    grep -aq "gicc ->pa : 0x0000000008010000" "$2"
    grep -aq "mmu probe : ESR=0x0000000096000006 (translation fault, level 2) ok" "$2"
    grep -aq "aarch64-mmu-ok" "$2"
    ! grep -aq "aarch64-mmu-BAD" "$2"

    # 想定外の例外を踏んでいないこと
    ! grep -aq "aarch64-exception-BAD" "$2"
}

run_one "virt" LOGs/aarch64-serial.log 512M
check_one "EL1 起動" LOGs/aarch64-serial.log 0x0000000020000000

run_one "virt,virtualization=on" LOGs/aarch64-serial-el2.log 512M
check_one "EL2 起動 -> 降格" LOGs/aarch64-serial-el2.log 0x0000000020000000

# **DTB を本当に読んでいるかの確認。** RAM を 1GB にすると DTB の memory も
# 1GB になる。直書きの 512MB のままなら追随できないので、ここで落ちる。
# ram end-1 = 0x7fffffff まで張れていることまで見る (マップも追随している証拠)
run_one "virt" LOGs/aarch64-serial-1g.log 1G
check_one "RAM 1GB (DTB 追随)" LOGs/aarch64-serial-1g.log 0x0000000040000000
grep -aq "ram end-1 : 0x000000007fffffff" LOGs/aarch64-serial-1g.log

echo "aarch64 smoke test: PASS (M0 + M1 + M2 + M2b, EL1 / EL2 降格 / RAM 1GB の 3 通り)"
