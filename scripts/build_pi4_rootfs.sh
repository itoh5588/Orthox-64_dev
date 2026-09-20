#!/bin/bash
# Raspberry Pi 4 の実機に載せる rootfs (xv6fs) を作る。
#
# **これまで手作業で組んでいた。**日報2026-08-16 §9-1 の宿題。
# out/pi4-ash-fs をその場で作っていたので、何が入っているのか
# リポジトリからは分からなかった。
#
#   scripts/build_pi4_rootfs.sh [出力イメージ]
#
# 出来たイメージは **p3 に dd する** (日報2026-08-15 §6)。
# WSL からは SD の生デバイスが見えないので、**Pi 側で書く**:
#
#   1. boot パーティションに rootfs.img としてコピー (Windows から書ける)
#   2. Raspberry Pi OS を起動して
#      sudo dd if=/boot/firmware/rootfs.img of=/dev/mmcblk0p3 bs=1M conv=fsync
#   **of= を /dev/mmcblk0 にしないこと。**パーティションテーブルごと飛ぶ
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-out/pi4-rootfs.img}"
FSDIR=out/pi4-rootfs-fs

BUSYBOX=out/busybox-aarch64-musl.elf
HELLO=out/aarch64-hello.elf
DOOM=out/doomgeneric-aarch64.elf
WAD=rootfs/doom1.wad
REBOOT=out/aarch64-reboot.elf
COWSTRESS=out/aarch64-cowstress.elf

[ -f "$BUSYBOX" ] || { echo "missing $BUSYBOX ('make aarch64-busybox-musl')" >&2; exit 1; }
# **`/reboot` を必ず入れる。**scripts/pi4/redeploy_and_smoke.sh が実機の
# ash から叩いて再起動させるのに使う。無いと netboot 経由の再配布が
# 「電源入れ直し」の手作業に戻ってしまう (2026-09-16、この rootfs だけ
# 入れ忘れて redeploy_and_smoke.sh を壊した)
[ -f "$REBOOT" ] || { echo "missing $REBOOT ('make aarch64-reboot')" >&2; exit 1; }

# **64MB。** p3 は 8.4GB あるので余裕はあるが、dd する量が増えると
# Pi 側の書き込み時間がそのまま伸びる。WAD を入れても 10MB 程度なので
# 64MB で足りる
XV6FS_BLOCKS="${XV6FS_BLOCKS:-65536}"
XV6FS_INODES="${XV6FS_INODES:-1024}"

rm -rf "$FSDIR"
mkdir -p "$FSDIR/bin" "$FSDIR/etc" "$FSDIR/tmp" "$FSDIR/dev"

# **applet は同じ ELF の別名コピーで置く。** busybox は argv[0] のベース名で
# applet を選ぶ。xv6fs のシンボリックリンク経路は当てにしない。
#
# **ash の組み込みで済むものも /bin に要る。** mkdir / rm / rmdir / sleep は
# 組み込みではないので、置き忘れると `rm: not found` になり
# 「fork/exec が壊れている」のと区別がつかない (日報2026-08-15 §12)
# **一覧は実機の /bin に合わせてある (2026-09-20)。**以前は 12 個しか置いて
# おらず、**この台本で組み直すと実機の中身を再現できなかった** —— awk も
# cowstress も入らない。2026-09-20 に実機で awk を直したとき、rootfs を
# 焼き直す道が採れなかった理由がこれ (日報2026-09-20 §11.1)。
#
# ★ **ハードリンクで置く。**build_rootfs_xv6fs.py がホスト側のハードリンクを
# 1 inode に集約するので、437KB の busybox が applet の数だけ増えずに済む
# (実機の /bin/awk も nlink=63 の 1 inode)。cp で置くと別 inode になり、
# 60 個で 26MB 余計に食う
cp "$BUSYBOX" "$FSDIR/bin/busybox"
for applet in ash awk basename cat chmod cmp comm cp cut date dd df diff \
              dirname du echo egrep env expr false fgrep find grep gunzip \
              gzip head hexdump install ln ls md5sum mkdir mktemp mv od \
              printf pwd readlink realpath rev rm rmdir sed seq sh sleep \
              sort stat sync tail tar tee test touch tr true uname uniq \
              wc which xargs yes; do
    ln -f "$FSDIR/bin/busybox" "$FSDIR/bin/$applet"
done

[ -f "$HELLO" ] && cp "$HELLO" "$FSDIR/bin/hello"

# **fork の CoW の負荷試験。**実機で CoW を叩くのに要る (日報2026-09-19 §1)。
# 無ければ黙って飛ばす —— make aarch64-cowstress で作れる
if [ -f "$COWSTRESS" ]; then
    cp "$COWSTRESS" "$FSDIR/bin/cowstress"
    echo "cowstress を入れた (/bin/cowstress)"
else
    echo "cowstress は入れない (make aarch64-cowstress で作れる)"
fi

cp "$REBOOT" "$FSDIR/reboot"
chmod +x "$FSDIR/reboot"

# **DOOM と WAD。** 無ければ黙って飛ばす — ash だけの rootfs も作れる
if [ -f "$DOOM" ] && [ -f "$WAD" ]; then
    cp "$DOOM" "$FSDIR/bin/doom"
    cp "$WAD"  "$FSDIR/doom1.wad"
    echo "DOOM を入れた (/bin/doom と /doom1.wad)"
else
    echo "DOOM は入れない (make aarch64-doom で作れる)"
fi

printf 'hello from aarch64 xv6fs rootfs\n' > "$FSDIR/etc/motd"

# **カーネルの起動時自己診断が中身まで照合する既知ファイル。**
# 入れないと fs selftest が read file : BAD を出す
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$FSDIR/aarch64-m4.txt"

# ---- ソース木の刻印 (S-12) -------------------------------------------------
# 焼いた後にソースを直すと rootfs だけが古いままになる。ビルド時点の
# git commit を刻んでおき、scripts/verify_rootfs_source.sh で dd の前に
# リポジトリの現在地と突き合わせられるようにする。
GIT_COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY="$([ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] && echo 1 || echo 0)"
printf 'commit=%s\ndirty=%s\nbuilt_at=%s\n' \
    "$GIT_COMMIT" "$GIT_DIRTY" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    > "$FSDIR/etc/orthox-build-info"

rm -f "$OUT"
XV6FS_FSSIZE="$XV6FS_BLOCKS" XV6FS_NINODES="$XV6FS_INODES" \
    python3 scripts/build_rootfs_xv6fs.py "$FSDIR" "$OUT"

echo
echo "=== できた ==="
ls -l "$OUT"
echo
echo "p3 への書き方は日報2026-08-15 §6 (Pi 側で dd)"
