#!/bin/bash
# Orthox aarch64 の**セルフホスト用 rootfs イメージ**を作る。
#
#   出力: out/rootfs-aarch64-selfhost.img  (既定 320MB)
#
# 目的は「OS が自分のカーネルをビルドする」(kernel-native-build: PASS)。
# そのために OS の中へ入れるものは 3 つ:
#
#   (a) カーネルのソース          /src/kernel-build   **入る (この台本)**
#   (b) OS 上で動く cc1 / as / ld /bin, /usr/bin      **まだ無い**
#   (c) busybox ash               /bin/ash            **まだ無い**
#                                 (make が /bin/sh を呼ぶ)
#
# **(b)(c) が揃うまでこのイメージ単体ではビルドできない。** それでも先に
# 作るのは、(a) の搬入と「大きいイメージが本当に扱えるか」を切り離して
# 確かめるため。実測では 4MB -> 320MB の 80 倍でドライバも xv6fs も
# そのまま通った (capacity 0xa0000 セクタを認識、mount ok、exec ok)。
#
# **既存の out/rootfs-*.img には触らない。** 特に
# out/rootfs-gcc-selfhost.img (riscv64、1GB) は作り直しが高いので消さないこと。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

IMG="${IMG:-out/rootfs-aarch64-selfhost.img}"
FSDIR="${FSDIR:-out/aarch64-selfhost-fs}"
SRCTREE="${SRCTREE:-build/aarch64-native-src}"

# 1KB ブロック単位。320MB。ツールの既定と同じ。
# ツールキットを入れると足りなくなるはずなので、そのときは
#   FSSIZE=1048576 NINODES=32768   (1GB。riscv64 の selfhost と同じ)
# に上げる。**カーネル側の上限ではない** — xv6fs は三重間接まで持っており
# 1 ファイル 16GB まで扱える (include/xv6fs.h:38)
FSSIZE="${FSSIZE:-327680}"
NINODES="${NINODES:-8192}"

case "$IMG" in
  out/rootfs-gcc-selfhost.img|out/rootfs.img)
    echo "error: $IMG は既存の成果物。作り直しが高いので拒否する" >&2; exit 1;;
esac

# ---- カーネルソースを組み立てる ------------------------------------------
echo "--- カーネルソースを staging"
bash scripts/stage_aarch64_native_build.sh "$ROOT/$SRCTREE"

# ---- イメージの中身を並べる ----------------------------------------------
echo "--- イメージの中身を並べる"
rm -rf "$FSDIR"
mkdir -p "$FSDIR/bin" "$FSDIR/src"

# **カーネルが中身まで照合する既知のファイル。**
# 「読めた」だけでは、別のブロックを返していても気づけない
# (tests/aarch64_smoke.sh と同じ文字列。fs の自己診断がこれを見る)
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$FSDIR/aarch64-m4.txt"

# P1 の確認用。**ツールチェーンが入るまでは、これが唯一の実行可能ファイル。**
# 起動が通っていることをイメージ単体で確かめられるようにしておく
if [ -f out/aarch64-hello.elf ]; then
  cp out/aarch64-hello.elf "$FSDIR/bin/hello"
else
  echo "warning: out/aarch64-hello.elf が無い。先に make aarch64-user-bin" >&2
fi

cp -a "$SRCTREE" "$FSDIR/src/kernel-build"

# ---- イメージを焼く -------------------------------------------------------
echo "--- イメージを焼く (FSSIZE=$FSSIZE blocks / NINODES=$NINODES)"
rm -f "$IMG"
XV6FS_FSSIZE="$FSSIZE" XV6FS_NINODES="$NINODES" \
  python3 scripts/build_rootfs_xv6fs.py "$FSDIR" "$IMG"

# ---- 配線証明 -------------------------------------------------------------
# **イメージの不備は QEMU を 1 往復してからしか出ない。**
# ここで潰せるものは潰す (stage_aarch64_native_build.sh と同じ方針)

want_bytes=$((FSSIZE * 1024))
got_bytes="$(stat -c %s "$IMG")"
[ "$got_bytes" = "$want_bytes" ] || {
  echo "★ イメージのサイズが $got_bytes で $want_bytes と合わない" >&2; exit 1; }
echo "  サイズ $((got_bytes / 1024 / 1024)) MB  ok"

# xv6fs のスーパーブロック magic (block 1 の先頭 4 バイト、リトルエンディアン)
magic="$(od -An -tx4 -j1024 -N4 "$IMG" | tr -d ' \n')"
[ "$magic" = "10203040" ] || {
  echo "★ superblock magic が 0x$magic (0x10203040 でない)" >&2; exit 1; }
echo "  superblock magic 0x$magic  ok"

# **中身が入っていること。** 空のイメージでも上の 2 つは通るので効く
n_src="$(find "$FSDIR/src/kernel-build" -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \) | wc -l)"
[ "$n_src" -ge 100 ] || { echo "★ カーネルソースが $n_src 本しか入っていない" >&2; exit 1; }
[ -f "$FSDIR/src/kernel-build/Makefile" ] || {
  echo "★ ネイティブ Makefile が入っていない" >&2; exit 1; }
echo "  カーネルソース $n_src 本 + Makefile  ok"

echo
echo "=== 完了: $IMG ==="
ls -la "$IMG"
echo
echo "起動して確かめる (**イメージを書き換えないよう -snapshot を付けること**):"
echo "  qemu-system-aarch64 -machine virt,virtualization=on -cpu cortex-a72 \\"
echo "    -m 512M -smp 1 -nographic -snapshot -kernel out/kernel-aarch64.elf \\"
echo "    -drive file=$IMG,if=none,format=raw,id=vblk0 \\"
echo "    -device virtio-blk-device,drive=vblk0"
echo
echo "**まだ OS の中でビルドはできない。** /bin/ash と cc1/as/ld が要る (冒頭 (b)(c))。"
