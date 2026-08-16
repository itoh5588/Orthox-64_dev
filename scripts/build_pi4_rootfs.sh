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

[ -f "$BUSYBOX" ] || { echo "missing $BUSYBOX ('make aarch64-busybox-musl')" >&2; exit 1; }

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
for applet in ash echo cat wc uname sort ls mkdir rm rmdir sleep grep; do
    cp "$BUSYBOX" "$FSDIR/bin/$applet"
done

[ -f "$HELLO" ] && cp "$HELLO" "$FSDIR/bin/hello"

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

rm -f "$OUT"
XV6FS_FSSIZE="$XV6FS_BLOCKS" XV6FS_NINODES="$XV6FS_INODES" \
    python3 scripts/build_rootfs_xv6fs.py "$FSDIR" "$OUT"

echo
echo "=== できた ==="
ls -l "$OUT"
echo
echo "p3 への書き方は日報2026-08-15 §6 (Pi 側で dd)"
