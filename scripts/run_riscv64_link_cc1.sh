#!/bin/bash
# Orthox 上で cc1 をリンクする。
#
#   scripts/run_riscv64_link_cc1.sh
#
# .o は既に出来ている前提で、アーカイブを作り直して cc1 をリンクするところだけを行う。
# 失敗しても .o は無事なので、この工程だけ何度でもやり直せる。
#
# **固定 sleep で送り込まないこと。** 工程ごとに所要が 2 分から 40 分まで振れるので、
# 短ければ次のコマンドが前の出力に紛れ、長ければ丸ごと待ち時間になる。
# ここでは各コマンドの完了マーカーがログに出るまで待ってから次を送る。
#
# 環境変数で上書きできる: IMG KERNEL BIOS LOGDIR MEM
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG=${IMG:-$ROOT/out/rootfs-riscv64-xv6.img}
KERNEL=${KERNEL:-$ROOT/out/kernel-riscv64.elf}
BIOS=${BIOS:-/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin}
LOGDIR=${LOGDIR:-$ROOT/logs/link}
MEM=${MEM:-2048M}

mkdir -p "$LOGDIR"
LOG=$LOGDIR/link.log
FIFO=$LOGDIR/.in.$$
: > "$LOG"

for f in "$IMG" "$KERNEL" "$BIOS"; do
    [ -f "$f" ] || { echo "見つからない: $f" >&2; exit 1; }
done

say() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOGDIR/driver.log"; }

rm -f "$FIFO"
mkfifo "$FIFO" || exit 1

qemu-system-riscv64 -machine virt -cpu rv64 -m "$MEM" -smp 1 \
    -bios "$BIOS" -kernel "$KERNEL" -display none -serial stdio -monitor none \
    -drive "file=$IMG,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 < "$FIFO" > "$LOG" 2>&1 &
QPID=$!
exec 9>"$FIFO"
rm -f "$FIFO"

cleanup() { exec 9>&- 2>/dev/null; kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

send() { printf '%s\n' "$1" >&9; }

# **時間切れでも即座に殺さないこと。** xv6fs のログが確定していない書き込みは
# 強制終了で消える。まず sync と exit を送り、それでも駄目なら trap が殺す
bail() {
    say "中断: $1"
    send 'sync'
    sleep 30
    send 'exit'
    sleep 20
    exit 1
}

# イメージ内のファイルサイズ (無ければ 0)
img_size() {
    python3 "$ROOT/scripts/build_rootfs_xv6fs.py" --stat "$1" "$IMG" 2>/dev/null \
        | awk '/^  size:/ { print $2; found=1 } END { if (!found) print 0 }'
}

# マーカーがログに出るまで待つ。QEMU が死んだら即座に諦める
waitfor() {
    local marker=$1 limit=$2 start=$SECONDS
    while :; do
        if grep -aq -- "$marker" "$LOG"; then
            say "  $marker ($((SECONDS - start)) 秒)"
            return 0
        fi
        if ! kill -0 "$QPID" 2>/dev/null; then
            say "  QEMU が終了した ($marker を待っている途中)"
            return 2
        fi
        if [ $((SECONDS - start)) -ge "$limit" ]; then
            say "  時間切れ: $marker を ${limit} 秒待った"
            return 1
        fi
        sleep 5
    done
}

say "開始  img=$IMG"
sleep 12
send 'export PATH=/bin:/usr/bin'
sleep 2
send 'cd /src/gcc-full/build/gcc'
sleep 2

# 1. 未完成の .o を作る (done.txt に無いものだけが対象。通常は lto-compress の 1 本)
say "1. 残りの .o"
send 'sh build_cc1.sh'
waitfor 'CC1BUILD-DONE' 3600 || bail 'CC1BUILD-DONE'

# 2. libbackend.a。**80 個ずつに割る。** exec の引数は 128 個で頭打ちなので
#    ar rcs libbackend.a *.o と書くと超えた分が黙って捨てられる
say "2. libbackend.a"
BACKEND_SIZE=$(img_size /src/gcc-full/build/gcc/libbackend.a)
if [ "${BACKEND_SIZE:-0}" -gt 1000000 ]; then
    say "  既に $BACKEND_SIZE バイト。作り直さない (中身はホスト側で ar t 済み)"
else
send 'rm -f libbackend.a; b=""; i=0; for n in $(cat objlist.txt); do b="$b $n.o"; i=$((i+1)); if [ $((i%80)) = 0 ]; then ar q libbackend.a $b; b=""; fi; done; [ -n "$b" ] && ar q libbackend.a $b; ranlib libbackend.a; echo "ARD""ONE"'
waitfor 'ARDONE' 7200 || bail 'ARDONE'
fi
# **中身の検査はゲスト内でやらないこと。** ar t は 28MB のアーカイブに対して
# 10 分以上かかり、待ち切れずに QEMU を殺して 1 度進行を失った。
# ホスト側なら --extract して ar t で一瞬で数えられる (0.5 秒)

say "3. libcommon.a / libcommon-target.a"
send 'rm -f libcommon.a; ar q libcommon.a diagnostic.o pretty-print.o intl.o input.o version.o; ranlib libcommon.a; echo "LC-""OK"'
waitfor 'LC-OK' 1800 || bail 'LC-OK'
send 'rm -f libcommon-target.a; ar q libcommon-target.a riscv-common.o prefix.o params.o opts.o opts-common.o options.o vec.o hooks.o common-targhooks.o; ranlib libcommon-target.a; echo "LCT-""OK"'
waitfor 'LCT-OK' 1800 || bail 'LCT-OK'

# 4. リンク。**先に cc1 を消すこと。** 前回の残骸があると
#    ls -l cc1 が成功して「出来た」に見える (実際に 948977 バイトの残骸があった)
say "4. cc1 のリンク"
send 'rm -f cc1; echo "RM-""OK"'
waitfor 'RM-OK' 600 || bail 'RM-OK'
send '/usr/bin/gcc -static -o cc1 c-lang.o c-family/stub-objc.o attribs.o c-errors.o c-decl.o c-typeck.o c-convert.o c-aux-info.o c-objc-common.o c-parser.o tree-mudflap.o c-family/c-common.o c-family/c-cppbuiltin.o c-family/c-dump.o c-family/c-format.o c-family/c-gimplify.o c-family/c-lex.o c-family/c-omp.o c-family/c-opts.o c-family/c-pch.o c-family/c-ppoutput.o c-family/c-pragma.o c-family/c-pretty-print.o c-family/c-semantics.o c-family/c-ada-spec.o default-c.o cc1-checksum.o main.o libbackend.a libcommon-target.a libcommon.a ../libcpp/libcpp.a ../libdecnumber/libdecnumber.a ../libiberty/libiberty.a -L../../prereq/lib -lmpc -lmpfr -lgmp -L../zlib -lz; echo "LINK-""RC=$?"'
waitfor 'LINK-RC=' 7200 || bail 'LINK-RC='
send 'ls -l cc1 && echo "CC1-""BUILT" || echo "CC1-""FAILED"'
waitfor 'CC1-BUILT\|CC1-FAILED' 1200 || bail 'CC1-BUILT'

# 5. **必ず正常に落とすこと。** 強制終了すると xv6fs のログが確定せず、
#    書いたアーカイブが 0 バイトに戻る (実際に 1 度失った)
say "5. 後始末"
send 'sync'
sleep 20
send 'exit'
sleep 15

say "終了"
grep -aE "LC-OK|LCT-OK|LINK-RC=|CC1-BUILT|CC1-FAILED|undefined reference|cannot find" "$LOG" | tail -20
