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
# M4 の確認用ディスク。**新しい名前を使う。** out/rootfs-*.img には触らない
# (日報2026-08-09 D: rootfs-gcc-selfhost.img を消さないこと)
TEST_DISK=out/aarch64-test-disk.img
TEST_FSDIR=out/aarch64-test-fs
#
# **1 枚のディスクで M4-1/2 (生の read/write) と M4-3 (xv6fs) の両方を見る。**
# 別々にすると、ドライバが 2 台目を選べないので回を分けることになる。
# 同居できるのは、xv6fs の block 0 (= LBA 0-1) がブートブロックで
# 未使用だから。そこに生の目印を置いても fs は壊れない。
#
#   LBA 0-1    xv6fs の block 0 (未使用) に M4-1/2 の目印を焼く
#   block 1-   xv6fs の superblock 以降
#   末尾       fs の外に 64KB の余白。**書き込み探針はここへ書く**
#              (以前は LBA 4 に書いていた = block 2 = ログ領域を壊す)
XV6FS_TEST_BLOCKS=4096          # 1KB ブロック x 4096 = 4MB
XV6FS_TEST_SLACK=65536          # fs の後ろの余白
make_test_disk() {
    mkdir -p out
    rm -rf "$TEST_FSDIR"
    mkdir -p "$TEST_FSDIR"
    # **カーネルが中身まで照合する既知のファイル。**
    # 「読めた」だけでは、別のブロックを返していても気づけない
    printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
    # **最初のユーザープログラム (P1)。** カーネルに埋め込まず、
    # ディスクから読ませる — ELF の読み込み経路ごと確かめるため
    mkdir -p "$TEST_FSDIR/bin"
    cp out/aarch64-hello.elf "$TEST_FSDIR/bin/hello"
    rm -f "$TEST_DISK"
    XV6FS_FSSIZE=$XV6FS_TEST_BLOCKS XV6FS_NINODES=256 \
        python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null
    python3 - "$TEST_DISK" "$XV6FS_TEST_SLACK" <<'PYEOF'
import sys
path, slack = sys.argv[1], int(sys.argv[2])
buf = bytearray(open(path, "rb").read())
# block 0 (LBA 0-1) は xv6fs では未使用。生の目印を焼く。
# 別々の文字列にするのは「LBA を無視して同じ場所を読んでいる」を弾くため
buf[0:24]    = b"ORTHOX-AARCH64-M4-SEC000"
buf[512:536] = b"ORTHOX-AARCH64-M4-SEC001"
buf += bytearray(slack)          # fs の外の余白。書き込み探針の行き先
open(path, "wb").write(buf)
PYEOF
}

run_one() {  # $1 = -machine、$2 = ログ、$3 = -m、$4 = 非空ならディスクを付ける
    local machine="$1" log="$2" mem="${3:-512M}" disk="${4:-}"
    local drive_args=()
    rm -f "$log"
    if [ -n "$disk" ]; then
        make_test_disk
        drive_args=(-drive "file=$TEST_DISK,if=none,format=raw,id=vblk0"
                    -device virtio-blk-device,drive=vblk0)
    fi
    "$QEMU_BIN" \
        -machine "$machine" \
        -cpu cortex-a72 \
        -m "$mem" \
        -smp 1 \
        -nographic \
        "${drive_args[@]}" \
        -kernel "$KERNEL" < /dev/null > "$log" 2>&1 &
    QEMU_PID=$!
    for _ in {1..120}; do
        if grep -aqE "aarch64-fs-(ok|BAD|none)" "$log" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=""

    # 終了マーカーは **最後に出るもの** にすること。途中の行にすると、
    # その後ろの出力を待たずに QEMU を止めてしまう
    # (M3c-2b / M4-3 で判定を末尾に足すたびにここを動かしている)
    #
    # **打ち切りを黙って見逃さない。** マーカーが出ないまま止めた場合、
    # 以降の判定は「中身が違う」ではなく「途中で切れた」で落ちる。
    # それを取り違えると、存在しないバグを探すことになる
    if ! grep -aqE "aarch64-fs-(ok|BAD|none)" "$log" 2>/dev/null; then
        echo "*** 実行が終わる前に打ち切られた ($log)" >&2
        echo "*** 最後の 5 行:" >&2
        tail -5 "$log" >&2
        exit 1
    fi
}

# **否定判定はこの関数を通すこと。**
#
# `! grep ...` は set -e の対象外 (POSIX: "return value is being inverted
# with !")。**15 か所すべてが、何が出ていても素通りしていた。**
# 最小再現で確かめた:
#
#   set -e; ! grep -q hit file   # パターンが有っても次の行へ進む
#
# 「出てはいけないもの」を見張るつもりの行が、1 つも見張っていなかった。
must_not() {   # $1 = 固定文字列, $2 = ログ, $3 = 説明 (任意)
    if grep -aqF -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た: $1  ($2)" >&2
        [ -n "${3:-}" ] && echo "*** $3" >&2
        grep -aF -- "$1" "$2" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

must_not_re() {  # $1 = 拡張正規表現, $2 = ログ, $3 = 説明 (任意)
    if grep -aqE -- "$1" "$2"; then
        echo "*** 出てはいけないものが出た (regex): $1  ($2)" >&2
        [ -n "${3:-}" ] && echo "*** $3" >&2
        grep -aE -- "$1" "$2" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

check_one() {  # $1 = 見出し、$2 = ログ、$3 = 期待する RAM 容量、$4 = RAM 末尾
    local ram_size="${3:-0x0000000020000000}"
    local ram_last="${4:-0x000000005fffffff}"
    echo "--- AArch64 Serial Output ($1) ---"
    cat "$2"
    echo "----------------------------------"

    # M0: 起動して PL011 に出る
    grep -aq "Orthox-64 aarch64 boot" "$2"
    # EL2 で始まっても降格して EL1 に居ること。EL2 のままだと tick を取りこぼす
    grep -aq "CurrentEL : EL1" "$2"
    grep -aq "bss zero  : ok" "$2"        # start.S のゼロ埋めが効いていること
    must_not "bss zero  : BAD" "$2"
    grep -aq "aarch64-boot-ok" "$2"

    # M2b: DTB
    #
    # **「値が正しい」だけでは DTB を読んだ証拠にならない。** 直書きの既定値と
    # 同じ値になるので、既定値のまま進んでいても出力は同じになる。実際、
    # 深さごとの状態を持たずに書いていたとき、intc の子ノード (v2m) に
    # 状態を潰されて GIC だけ既定値のまま進んでいた。
    # そこで **どこから来た値かを (dtb) / (既定値) で出させて判定する。**
    grep -aq "aarch64-dtb-ok" "$2"
    must_not "aarch64-dtb-BAD" "$2"
    grep -aq "memory    : 0x0000000040000000 size ${ram_size}  (dtb)" "$2"
    grep -aq "uart      : 0x0000000009000000  (dtb)" "$2"
    # GIC は reg[0] と reg[1] の**両方**が取れて初めて (dtb) になる
    grep -aq "gic dist  : 0x0000000008000000 size 0x0000000000010000  (dtb)" "$2"
    grep -aq "gic cpu   : 0x0000000008010000 size 0x0000000000010000  (dtb)" "$2"
    grep -aq "virtio    : 0x000000000a000000 x 0x0000000000000020 stride 0x0000000000000200  (dtb)" "$2"
    grep -aq "timer irq : 0x000000000000001e  (dtb)" "$2"   # PPI 14 + 16 = 30
    # **スロット i の INTID = base + i が DTB で確かめられていること。**
    # 確かめずに決め打ちすると、別のデバイスの割り込みを待つ
    grep -aq "virtio irq: 0x0000000000000030  (dtb、base + スロット番号で確認済み)" "$2"
    must_not "(既定値)" "$2"     # 1 つでも既定値に落ちていたら失格

    # M1: 例外ベクタ + GIC + generic timer で tick が入る
    #
    # 逆確認済み (日報2026-08-08):
    #   daifclr を外す        -> ticks 0 で timer-BAD を出して進む
    #   GIC で有効化しない    -> timer freq の次で止まり ticks 行すら出ない
    # 別々の壊れ方をするので、まとめて外さずに 1 つずつ確かめてある
    grep -aq "timer freq: 0x0000000003b9aca0" "$2"   # 62.5MHz (QEMU virt の実測値)
    grep -aq "aarch64-timer-ok" "$2"
    must_not "aarch64-timer-BAD" "$2"

    # M2 / M3b: MMU を入れて上位 VA (TTBR1) へ移る
    #
    # **恒等マッピングだけでは「MMU が効いている」証拠にならない。** VA == PA
    # なので、MMU を入れ忘れても出力はまったく同じになる。判定は 4 本立て:
    #
    #   1. SCTLR_EL1 の M / C / I が立っていること (レジスタの実測値)
    #   2. 未マップの VA を読んで translation fault が上がること (翻訳の証拠)
    #   3. **カーネルが上位 VA で走っていること** (pc / sp / VBAR が 0xffffff80...)
    #   4. **恒等マッピングを外した後も文字が出ること**
    #      = カーネルが TTBR1 だけで走っている証拠
    grep -aq "M2/M3b: MMU (4KB granule, VA 39bit, TTBR1 = カーネル)" "$2"
    # カーネルが自分で決めた値。**EL1 起動と EL2 降格で同じ値になること**が
    # 要点 (起動時の値を読んで OR していたときは別の値になっていた)
    grep -aq "SCTLR_EL1 : 0x0000000030d0181d" "$2"
    grep -aq "kernel VA : 0xffffff8040201000  (物理 0x0000000040201000)" "$2"
    # 有効化前の自前ウォーク。上位側 (TTBR1) と恒等側 (TTBR0) の両方を見る。
    # どちらが欠けても沈黙する: 恒等が欠ければ MMU on の瞬間、
    # 上位が欠ければ飛んだ瞬間
    grep -aq "ttbr1 uart: 0x0000000009000000" "$2"
    grep -aq "ttbr1 gicc: 0x0000000008010000" "$2"
    grep -aq "ttbr1 ram : ${ram_last}" "$2"
    grep -aq "ttbr0 iden: 0x0000000040201000" "$2"
    grep -aq "ttbr0 user: .*  ok (.user_text を指している)" "$2"
    # 上位 VA へ移れたこと。pc / sp / VBAR / UART が全部 0xffffff80... になる
    grep -aq "high VA   : pc=0xffffff80.* sp=0xffffff80" "$2"
    grep -aq "vbar/uart : 0xffffff80.* / 0xffffff8009000000" "$2"
    # **恒等を外した後の行。** ここが出れば TTBR1 だけで走っている
    grep -aq "恒等を外した。カーネルは TTBR1 だけで走っている" "$2"
    grep -aq "mmu probe : ESR=0x0000000096000006 (translation fault, level 2) ok" "$2"
    grep -aq "aarch64-mmu-ok" "$2"
    must_not "aarch64-mmu-BAD" "$2"

    # M3c-2a: 共有層 (pmm.h / arch_vm_*)
    #
    # **コンパイルが通ったことは動く証拠にならない。** 実際に共有層が使う
    # 経路だけを見る。とくに 2 つ:
    #
    #   hhdm      g_hhdm_offset が入っていること。**0 のままでも起動も
    #             タイマも MMU も通る**ので、出させないと気づけない
    #             (日報2026-08-09 追-7 と同じ形)
    #   free x1   参照カウントが効いていること。1 回目の pmm_free で
    #             返してしまうと、まだ使われているページを配り直す
    grep -aq "hhdm      : 0xffffff8000000000  ok" "$2"
    grep -aq "map/get   :  ok" "$2"     # arch_vm_map_page -> get_phys が往復する
    grep -aq "unmap     :  ok" "$2"     # 外した後は 0 を返す
    grep -aq "free x1   :  ok" "$2"     # 参照カウントが効いている
    grep -aq "aarch64-shared-ok" "$2"
    must_not "aarch64-shared-BAD" "$2"

    # M3a / M3b-2: EL0 + svc + プロセスごとのアドレス空間
    #
    # 判定は 5 本立て。**どれか 1 つでは足りない**:
    #
    #   1. EL0 から write が届く      遷移と svc の往復が成立している
    #   2. EL0 実行中に tick が入る   「下位 EL の IRQ」ベクタ (+0x480) が効いている
    #   3. EL0 からカーネルの .text を読むと permission fault になる
    #      **translation fault では駄目。** それは「張っていない」だけで、
    #      権限で弾いた証拠にならない
    #   4. **別々のアドレス空間で 2 回走らせて、marker が 2 回とも 0**
    #      データが空間ごとに私物になっている証拠。共有されていたら
    #      2 回目は 1 になる
    #   5. **カーネルの VA を write に渡すと -EFAULT で弾かれる**
    #      アドレス空間を分けただけでは防げない (カーネル自身は上位 VA を
    #      読めてしまう)。ポインタの検査が要る
    grep -aq "M3a/M3b/M3c: EL0 + アドレス空間 + コンテキストスイッチ" "$2"
    # **行が割れていないこと。** 並行に走る 2 本の出力は、1 行の途中で
    # 切り替わると混ざる (実測で 12 回中 5 回)。カーネルの動作自体は正しく
    # aarch64-user-ok も出るので、下の「数が 2 であること」だけだと
    # 「数が合わない」という遠回りな落ち方しかせず、原因が読めない。
    # EL0 の出力は必ず行頭 (空白の後) から始まるので、その前に何か
    # 出ていたら行が割れている
    must_not_re '[^ ].*\[EL0\]' "$2" "並行に走る 2 本の出力が 1 行の途中で混ざっている"
    # ユーザーは TTBR0 の VA (0x400000) で動く。**カーネルの配置と無関係。**
    grep -aq "user text : 0x0000000000400000" "$2"
    grep -aq "user sp   : 0x0000000000403000" "$2"
    # 同じプログラムが 2 回走っている
    [ "$(grep -ac "\[EL0\] hello from user mode" "$2")" = "2" ]
    [ "$(grep -ac "\[EL0\] resumed after permission fault" "$2")" = "2" ]
    # **2 回とも marker が 0** = データが私物
    [ "$(grep -ac "marker    : 0x0000000000000000  ok (私物のデータ)" "$2")" = "2" ]
    must_not "BAD (前の空間の値が見えている)" "$2"
    [ "$(grep -ac "svc calls : 0x0000000000000005" "$2")" = "2" ]
    [ "$(grep -ac "el0 ticks : .*  ok" "$2")" = "2" ]
    # EL0 から読ませるのはカーネルの**上位 VA**。EL0 も TTBR1 を歩けるが、
    # AP が EL1 だけなので permission fault になる
    [ "$(grep -ac "user probe: ESR=0x000000009200000f FAR=0xffffff8040201000 (permission fault) ok" "$2")" = "2" ]
    # -14 = -EFAULT
    [ "$(grep -ac "bad ptr   : 0xfffffffffffffff2  ok (-EFAULT で弾いた)" "$2")" = "2" ]
    # M3c-2a: フレームの sp_el0 が SP_EL0 に届いていること。
    # **SP_EL0 は例外で自動的に保たれる**ので、save も restore も外したまま
    # でも他の判定は全部通る (逆確認で実証)。カーネル側から意図的に
    # ずらして戻ってくるかを見て初めて効いている証拠になる
    [ "$(grep -ac "sp probe  : .*  ok (SP_EL0 がフレーム経由で効く)" "$2")" = "2" ]
    # M3c-1: コンテキストスイッチとプリエンプション
    #
    #   カーネルスレッド 2 本が両方 200 周する = 譲り合いが成立している
    #   切り替え回数が 0 でない            = 実際に切り替わっている
    #
    # **ユーザータスク 2 本は並行に走る。** 出力が交錯すること自体が、
    # タイマ割り込みで切り替わっている証拠 (どちらも hello を出してから
    # 片方が終わる)
    grep -aq "kthreads  : 0x00000000000000c8 / 0x00000000000000c8  ok (両方最後まで回った)" "$2"
    grep -aq "switches  : .*  ok" "$2"
    [ "$(grep -ac -- "--- 空間 A ttbr0=" "$2")" = "1" ]
    [ "$(grep -ac -- "--- 空間 B ttbr0=" "$2")" = "1" ]

    grep -aq "aarch64-user-ok" "$2"
    must_not "aarch64-user-BAD" "$2"

    # M3c-2b: 共有スケジューラ (kernel/sched.c) で実際に切り替わること
    #
    # **寝て起きる 1 往復に、確かめたいものが全部入っている。**
    # schedule / arch_context_switch / cpu_local / task_on_timer_tick /
    # task_poll_sleep_wakeups / task_consume_resched のどれか 1 つでも
    # 欠けたら戻ってこない (逆確認で、実際に止まることを確認した)
    grep -aq "Task system initialized." "$2"
    grep -aq "  current   :  ok" "$2"
    grep -aq "  idle task :  ok" "$2"
    grep -aq "  sleep     : .*  ok (寝て、タイマに起こされた)" "$2"
    grep -aq "aarch64-sched-ok" "$2"
    must_not "aarch64-sched-BAD" "$2"

    # M4-3: xv6fs。ディスクの有無で結果が変わるので、ここでは
    # 「壊れていない」ことだけを見る。中身の判定はディスク付きの回で行う
    must_not "aarch64-fs-BAD" "$2"

    # M4: virtio-blk。**デバイスの有無で結果が変わるので、ここでは
    # 「壊れていない」ことだけを見る。** 中身の判定はディスク付きの回で行う
    must_not "aarch64-virtio-BAD" "$2"

    # 想定外の例外を踏んでいないこと
    must_not "aarch64-exception-BAD" "$2"
}
run_all() {
run_one "virt" LOGs/aarch64-serial.log 512M
check_one "EL1 起動" LOGs/aarch64-serial.log 0x0000000020000000 0x000000005fffffff

run_one "virt,virtualization=on" LOGs/aarch64-serial-el2.log 512M
check_one "EL2 起動 -> 降格" LOGs/aarch64-serial-el2.log 0x0000000020000000 0x000000005fffffff

# **DTB を本当に読んでいるかの確認。** RAM を 1GB にすると DTB の memory も
# 1GB になる。直書きの 512MB のままなら追随できないので、ここで落ちる。
# ram end-1 = 0x7fffffff まで張れていることまで見る (マップも追随している証拠)
run_one "virt" LOGs/aarch64-serial-1g.log 1G
check_one "RAM 1GB (DTB 追随)" LOGs/aarch64-serial-1g.log 0x0000000040000000 0x000000007fffffff

# M4: virtio-blk を付けて起動する。**付けない 3 通りでは
# aarch64-virtio-none になる** (デバイスが無いことを正しく検出している)
run_one "virt" LOGs/aarch64-serial-disk.log 512M with-disk
check_one "virtio-blk 付き" LOGs/aarch64-serial-disk.log 0x0000000020000000 0x000000005fffffff
echo "--- M4 の判定 ---"
# **「見つかった」だけでは証拠にならない。** 読んだ中身まで見る
grep -aq "read lba0 : \"ORTHOX-AARCH64-M4-SEC000\"  ok" LOGs/aarch64-serial-disk.log
# LBA 0 と違う中身が返ること = LBA が効いている
grep -aq "read lba1 : \"ORTHOX-AARCH64-M4-SEC001\"  ok (LBA が効いている)" LOGs/aarch64-serial-disk.log
grep -aq "write/read: ok (書いたものが読み戻せた)" LOGs/aarch64-serial-disk.log
# **読み書きが通っただけでは、割り込みで完了した証拠にならない。**
# ポーリングでも同じ結果になる。待ちを割り込みの印だけにしてあるので、
# 回数が 0 でなければ本当に割り込みで抜けている
grep -aq "intid     : 0x000000000000004f  (割り込みで完了を待つ)" LOGs/aarch64-serial-disk.log
grep -aq "irq count : .*  ok (割り込みで完了した)" LOGs/aarch64-serial-disk.log
grep -aq "aarch64-virtio-ok" LOGs/aarch64-serial-disk.log
must_not "aarch64-virtio-BAD" LOGs/aarch64-serial-disk.log

echo "--- M4-3 の判定 (xv6fs) ---"
# **マウントできただけでは、中身を辿れる証拠にならない。**
# イメージに入れておいた既知のファイルを、中身まで照合させる
grep -aq "xv6fs: mounted vblk0" LOGs/aarch64-serial-disk.log
grep -aq "  mount     :  ok" LOGs/aarch64-serial-disk.log
grep -aq "  read file :  ok" LOGs/aarch64-serial-disk.log
# 書けること = ログと bitmap が効いている。読み戻しまで見る
grep -aq "  write     :  ok" LOGs/aarch64-serial-disk.log
grep -aq "  read back :  ok" LOGs/aarch64-serial-disk.log
# **VFS / fd 層を通す経路 (C-1a)。** xv6fs 直叩きが通ったことは、
# タスクの fd テーブルとマウント解決を挟むこちらが通る証拠にならない。
# ユーザープログラムが通るのはこちらの道
grep -aq "  fd open   :  ok" LOGs/aarch64-serial-disk.log
grep -aq "  fd write  :  ok" LOGs/aarch64-serial-disk.log
grep -aq "  fd close  :  ok" LOGs/aarch64-serial-disk.log
# **開き直して読む。** 同じ fd から読み返すだけではディスクに届いた証拠にならない
grep -aq "  fd read   :  ok" LOGs/aarch64-serial-disk.log
# コンソール fd が本物であること (M3c-2b までは -1 を返すスタブだった)
grep -aq "  console fd:  ok" LOGs/aarch64-serial-disk.log
grep -aq "aarch64-fs-ok" LOGs/aarch64-serial-disk.log
# **戻り値の約束を守っていること。** storage の受け口が「読めたブロック数」を
# 返すと、xv6bio は ret != 0 をエラーとして記録するが**動きは正しいまま**。
# 判定はすべて ok のまま通り、エラーログだけが 8 行出る (逆確認で実測)。
# ここを見ないと、間違った約束のまま先へ進む
must_not "xv6bio: disk" LOGs/aarch64-serial-disk.log
# ディスクを付けない 3 通りでは aarch64-fs-none になること
# (「無い」と「壊れた」を混ぜない)
grep -aq "aarch64-fs-none" LOGs/aarch64-serial.log

echo "aarch64 smoke test: PASS (M0-M4, EL1 / EL2 降格 / RAM 1GB / virtio-blk の 4 通り)"
}

# ==========================================================================
# --self-test : **判定そのものが火を噴くかを確かめる**
#
# 日報2026-08-09 追9-6 で、否定判定 15 か所が全部空振りしていたことが
# 分かった (`! grep` は set -e の対象外)。must_not に直したが、
# **直したこと自体も確かめないと同じことが起きる** (追7-6 の一段先)。
#
# カーネルを 15 通りに壊すのは高くつく。**検査の実効性を見るなら、
# 入力のほうを作ればよい** — 通ったログに禁止文字列を 1 行足して、
# 落ちることを見る。
#
# 前提 (無傷のログが通ること) は直前の run_all が証明している。
# そこが落ちていればスモークはそこで終わっているので、ここには来ない。
# ==========================================================================
SELFTEST_LOG=LOGs/aarch64-selftest-inject.log

# check_one を回して終了コードだけ返す。**サブシェルで回す** —
# 中で exit されても呼び出し側は続けられる
selftest_check_one() {   # $1 = ログ
    ( check_one "self-test" "$1" 0x0000000020000000 0x000000005fffffff ) \
        > /dev/null 2>&1
}

selftest_must_not() {    # $1 = パターン, $2 = ログ
    ( must_not "$1" "$2" ) > /dev/null 2>&1
}

SELFTEST_PASS=0
SELFTEST_FAIL=0

selftest_probe() {  # $1 = 説明, $2 = 注入する行, $3 = 元ログ, $4 = 空なら check_one / 非空ならその must_not パターン
    cp "$3" "$SELFTEST_LOG"
    printf '%s\n' "$2" >> "$SELFTEST_LOG"
    local ok=0
    if [ -z "${4:-}" ]; then
        selftest_check_one "$SELFTEST_LOG" && ok=1
    else
        selftest_must_not "$4" "$SELFTEST_LOG" && ok=1
    fi
    if [ "$ok" = 1 ]; then
        echo "  [空振り] $1"
        SELFTEST_FAIL=$((SELFTEST_FAIL + 1))
    else
        echo "  [捕捉  ] $1"
        SELFTEST_PASS=$((SELFTEST_PASS + 1))
    fi
}

self_test() {
    local good_el1=LOGs/aarch64-serial.log
    local good_disk=LOGs/aarch64-serial-disk.log

    echo
    echo "=== --self-test: 否定判定が本当に火を噴くか ==="

    # 「無傷のログが通ること」は前提だが、**ここで確かめても必ず通る。**
    # 直前の run_all が同じログを同じ引数で check_one に通しており、
    # 落ちていればそこで終わっているため。番人を置いても火を噴く機会が
    # 無いので置かない (日報2026-08-09 追2-6 と同じ形。逆確認で実際に、
    # positive 判定を壊しても run_all が先に落ちることを確かめた)

    # check_one の中の 13 か所
    selftest_probe "bss zero  : BAD"               "  bss zero  : BAD"                    "$good_el1"
    selftest_probe "aarch64-dtb-BAD"               "aarch64-dtb-BAD"                      "$good_el1"
    selftest_probe "(既定値)"                       "  uart      : 0x9000000  (既定値)"     "$good_el1"
    selftest_probe "aarch64-timer-BAD"             "aarch64-timer-BAD"                    "$good_el1"
    selftest_probe "aarch64-mmu-BAD"               "aarch64-mmu-BAD"                      "$good_el1"
    selftest_probe "aarch64-shared-BAD"            "aarch64-shared-BAD"                   "$good_el1"
    selftest_probe "行の分断 ([EL0] が行頭でない)"  "  el0 ticks : 0x14  [EL0] resumed"     "$good_el1"
    selftest_probe "BAD (前の空間の値が見えている)" "  marker : 0x1  BAD (前の空間の値が見えている)" "$good_el1"
    selftest_probe "aarch64-user-BAD"              "aarch64-user-BAD"                     "$good_el1"
    selftest_probe "aarch64-sched-BAD"             "aarch64-sched-BAD"                    "$good_el1"
    selftest_probe "aarch64-fs-BAD"                "aarch64-fs-BAD"                       "$good_el1"
    selftest_probe "aarch64-virtio-BAD"            "aarch64-virtio-BAD"                   "$good_el1"
    selftest_probe "aarch64-exception-BAD"         "aarch64-exception-BAD"                "$good_el1"

    # ディスク付きの回だけで見ている 2 か所 (check_one の外)
    selftest_probe "M4 判定: aarch64-virtio-BAD"   "aarch64-virtio-BAD"  "$good_disk" "aarch64-virtio-BAD"
    selftest_probe "M4-3 判定: xv6bio: disk"       "xv6bio: disk read error: dev=vblk0 block=1 ret=2" \
                                                   "$good_disk" "xv6bio: disk"

    rm -f "$SELFTEST_LOG"
    echo "  捕捉 $SELFTEST_PASS / 空振り $SELFTEST_FAIL"
    if [ "$SELFTEST_FAIL" != 0 ]; then
        echo "*** 見張っていない判定がある" >&2
        exit 1
    fi
    echo "aarch64 smoke self-test: PASS (否定判定 $SELFTEST_PASS か所すべてが火を噴く)"
}

case "${1:-}" in
    --self-test)
        # **まず通常どおり回して、緑のログを作ってから注入する。**
        # 古いログを使うと「いつのログか」が分からなくなる
        run_all
        self_test
        ;;
    "")
        run_all
        ;;
    *)
        echo "usage: $0 [--self-test]" >&2
        exit 2
        ;;
esac
