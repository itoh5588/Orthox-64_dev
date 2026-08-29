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

# ---- busybox ash ----------------------------------------------------------
# **make が /bin/sh を呼ぶので必須。** applet は本体へのハードリンクにする
# (xv6fs に symlink 型が無い。riscv64 の RISCV64_ROOTFS_APPLETS と同じ形)。
# applet を増やしてもイメージは busybox 1 本分しか太らない。
BUSYBOX="${BUSYBOX:-$ROOT/out/busybox-aarch64-musl.elf}"
# **/bin に張っていない applet は「無い」のと同じ。**busybox に入って
# いても PATH から呼べない (2026-08-29 に GCC の configure 準備で気づいた
# —— sed / grep / awk はビルド済みなのに /bin に無かった)。
APPLETS="ash sh cat chmod cp echo env false head ln ls mkdir mv printf pwd rm \
         rmdir sleep stat tail test touch true wc which \
         awk sed grep egrep fgrep sort uniq cut tr tee comm diff \
         find xargs expr seq basename dirname readlink realpath \
         date uname du df dd sync mktemp install cmp md5sum od hexdump \
         rev yes tar gzip gunzip"
if [ -f "$BUSYBOX" ]; then
  echo "--- busybox ash を入れる"
  cp "$BUSYBOX" "$FSDIR/bin/busybox"
  for a in $APPLETS; do
    ln -f "$FSDIR/bin/busybox" "$FSDIR/bin/$a"
  done
  echo "  applet $(echo $APPLETS | wc -w) 本 (ハードリンク)"
else
  echo "warning: $BUSYBOX が無い。**/bin/sh が無いと make は動かない**" >&2
fi

# OS の中でコンパイルさせる種火。**中身まで照合できるように出力を固定する**
cat > "$FSDIR/selfhost-check.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("SELF-HOSTED-ON-ORTHOX-AARCH64\n"); return 0; }
EOF

# ---- ツールチェーンを重ねる ----------------------------------------------
# riscv64 の deploy と同じ形 (日報2026-08-01 追記 5):
#   make riscv64-rootfs RISCV64_ROOTFS_EXTRA=ports/orthox-native
# こちらは Makefile を経由せず、この台本の中で重ねる。
#
# EXTRA は **DESTDIR install したツリー**をそのまま重ねる想定
# (ports/orthox-native-aarch64 は prefix=/usr で install してあるので
#  usr/bin/as などが入っている)。
EXTRA="${EXTRA:-$ROOT/ports/orthox-native-aarch64}"
if [ -d "$EXTRA" ]; then
  echo "--- ツールチェーンを重ねる: $EXTRA"
  cp -a "$EXTRA"/. "$FSDIR/"
  # musl のヘッダと libc.a も要る。**Orthox 上の gcc は /include と /lib を見る**
  # (--with-sysroot=/ --with-native-system-header-dir=/include。日報2026-08-01)
  if [ -d "$ROOT/ports/musl-install-aarch64" ]; then
    mkdir -p "$FSDIR/include" "$FSDIR/lib"
    cp -a "$ROOT/ports/musl-install-aarch64/include"/. "$FSDIR/include/"
    cp -a "$ROOT/ports/musl-install-aarch64/lib"/.     "$FSDIR/lib/"

    # **xv6fs に symlink 型は無い (S-11、2026-08-29)。**
    # build_rootfs_xv6fs.py:403 は symlink を「リンク先の文字列を中身に持つ
    # 普通のファイル」に変換する。musl の ld-musl-aarch64.so.1 は libc.so を
    # 指す symlink なので、そのまま焼くと **71 バイトのテキストファイル**が
    # /lib/ld-musl-aarch64.so.1 になり、**動的リンクが黙って壊れる**
    # (ELF として読めず、exec が意味の分からない失敗をする)。
    # tests/aarch64_dynlink_smoke.sh は最初から実体で置いている。**揃える。**
    # **実機の gcc は glibc の名前でインタプリタを書く (2026-08-29)。**
    #
    #   Exec: Interpreter not found: /lib/ld-linux-aarch64.so.1
    #
    # ports/orthox-native-aarch64 の GCC は既定の dynamic-linker のまま
    # 組んであるので、PT_INTERP が /lib/ld-linux-aarch64.so.1 になる。
    # **musl のローダは名前が違うだけで中身は正しく動く**ので、その名前でも
    # 置いておく。これが無いと、libc.so を置いた途端 (= 既定が動的リンクに
    # 変わった途端) **実機で作ったバイナリが軒並み起動しなくなる。**
    # GCC の configure が「cannot run C compiled programs」で止まって露見した。
    if [ -f "$FSDIR/lib/libc.so" ]; then
      cp "$FSDIR/lib/libc.so" "$FSDIR/lib/ld-linux-aarch64.so.1"
      echo "  glibc 名のローダも置いた: ld-linux-aarch64.so.1"
    fi

    for l in "$FSDIR/lib"/*; do
      [ -L "$l" ] || continue
      t="$(readlink -f "$l")"
      rm -f "$l"
      if [ -f "$t" ]; then
        cp "$t" "$l"
        echo "  symlink を実体化: $(basename "$l") <- $(basename "$t")"
      else
        echo "★ $l の実体が無い ($t)" >&2; exit 1
      fi
    done
  fi
  du -sh "$FSDIR" | sed 's/^/  重ねた後: /'
else
  echo "--- ツールチェーンは無い ($EXTRA)。ソースだけのイメージになる"
fi

# ---- 動的リンクの検証用 (S-11 / DL-6) --------------------------------------
# **libc.so を置くだけでは「置いた」ことしか言えない。**実機で動くことを
# 確かめられるように、QEMU で PASS している一式 (日報2026-08-28 §15) を
# 同じ経路で載せる。make aarch64-dynlink-smoke が out/aarch64-dyn に作る。
DYN="${DYN:-$ROOT/out/aarch64-dyn}"
if [ -d "$DYN" ] && ls "$DYN"/*.elf >/dev/null 2>&1; then
  echo "--- 動的リンクの検証用を入れる: $DYN"
  cp "$DYN"/*.elf "$FSDIR/bin/"
  cp "$DYN"/*.so  "$FSDIR/lib/"
  echo "  elf $(ls "$DYN"/*.elf | wc -l) 本 / so $(ls "$DYN"/*.so | wc -l) 本"
else
  echo "--- 動的リンクの検証用は無い ($DYN)。make aarch64-dynlink-smoke で作れる"
fi

# ---- GCC のソース (実機でセルフホストするため、任意) ----------------------
#
# **既定では入れない。**185 MB あり、普段のイメージ (320 MB) には入らない。
# 入れるときは大きさと inode 数も一緒に上げること:
#
#   GCCSRC=out/gcc-src-c-only FSSIZE=1572864 NINODES=32768 \
#     bash scripts/build_rootfs_aarch64_selfhost.sh
#
# out/gcc-src-c-only は ports/gcc-4.7.4-aarch64 から build-* / 他言語 /
# testsuite を落としたもの (929 MB -> 185 MB、5,051 ファイル)。
#
# gmp / mpfr / mpc は ports/gcc-prereq-aarch64 に aarch64 版が既にあるので、
# それを /usr の下に重ねて --with-gmp=/usr で見せる。
GCCSRC="${GCCSRC:-}"
if [ -n "$GCCSRC" ]; then
  [ -d "$GCCSRC" ] || { echo "★ GCCSRC が無い: $GCCSRC" >&2; exit 1; }
  echo "--- GCC のソースを入れる: $GCCSRC"
  mkdir -p "$FSDIR/src"
  cp -a "$GCCSRC" "$FSDIR/src/gcc-4.7.4"
  PREREQ_A="${PREREQ_A:-$ROOT/ports/gcc-prereq-aarch64}"
  if [ -d "$PREREQ_A" ]; then
    echo "--- gmp/mpfr/mpc (aarch64) を重ねる: $PREREQ_A"
    mkdir -p "$FSDIR/usr"
    cp -a "$PREREQ_A"/. "$FSDIR/usr/"
  else
    echo "★ $PREREQ_A が無い。GCC の configure は gmp を要求する" >&2; exit 1
  fi
  echo "  ソース $(find "$FSDIR/src/gcc-4.7.4" -type f | wc -l) ファイル"
  du -sh "$FSDIR" | sed 's/^/  ここまでの合計: /'
fi

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
# 2026-08-28 に mtime 拡張で 0x10203040 -> 0x10203041 に上げた。
# **include/xv6fs.h の XV6FS_FSMAGIC と揃っていること**
[ "$magic" = "10203041" ] || {
  echo "★ superblock magic が 0x$magic (0x10203041 でない)" >&2; exit 1; }
echo "  superblock magic 0x$magic  ok"

# **中身が入っていること。** 空のイメージでも上の 2 つは通るので効く
n_src="$(find "$FSDIR/src/kernel-build" -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \) | wc -l)"
[ "$n_src" -ge 100 ] || { echo "★ カーネルソースが $n_src 本しか入っていない" >&2; exit 1; }
[ -f "$FSDIR/src/kernel-build/Makefile" ] || {
  echo "★ ネイティブ Makefile が入っていない" >&2; exit 1; }
echo "  カーネルソース $n_src 本 + Makefile  ok"

# GCC を載せたときは、configure が最初に触るものが在ることを見る
if [ -n "$GCCSRC" ]; then
  for f in src/gcc-4.7.4/configure src/gcc-4.7.4/gcc/Makefile.in \
           usr/lib/libgmp.a usr/lib/libmpfr.a usr/lib/libmpc.a \
           usr/include/gmp.h bin/awk bin/sed bin/grep; do
    [ -e "$FSDIR/$f" ] || { echo "★ /$f がイメージに無い" >&2; exit 1; }
  done
  echo "  GCC のソースと gmp/mpfr/mpc と awk/sed/grep  ok"
fi

# **S-11: 動的リンクの土台が「実体で」入っていること。**
# symlink のまま焼くと 71 バイトのテキストになるが、イメージの大きさも
# superblock magic も通ってしまう。**焼いた後のイメージから取り出して
# 中身を見る** — 規則が在るだけでは中身の正しさは分からない
# (日報2026-08-27 §18 と同じ方針)。
if [ -f "$FSDIR/lib/libc.so" ]; then
  tmpd="$(mktemp -d)"
  for f in libc.so ld-musl-aarch64.so.1 ld-linux-aarch64.so.1; do
    python3 scripts/extract_rootfs_xv6fs.py "$IMG" "/lib/$f" "$tmpd/$f" >/dev/null || {
      echo "★ イメージから /lib/$f を取り出せない" >&2; rm -rf "$tmpd"; exit 1; }
    m="$(od -An -tx1 -N4 "$tmpd/$f" | tr -d ' \n')"
    [ "$m" = "7f454c46" ] || {
      echo "★ /lib/$f が ELF でない (先頭 $m)。symlink が実体化されていない" >&2
      rm -rf "$tmpd"; exit 1; }
  done
  sa="$(stat -c %s "$tmpd/libc.so")"; sb="$(stat -c %s "$tmpd/ld-musl-aarch64.so.1")"
  [ "$sa" = "$sb" ] || {
    echo "★ libc.so ($sa) と ld-musl-aarch64.so.1 ($sb) の大きさが違う" >&2
    rm -rf "$tmpd"; exit 1; }
  rm -rf "$tmpd"
  echo "  共有 musl /lib/libc.so と /lib/ld-musl-aarch64.so.1 (実体 $sa バイト)  ok"
fi

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
