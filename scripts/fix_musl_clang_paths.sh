#!/bin/bash
# musl の musl-clang / ld.musl-clang が埋め込むパスを、**この作業ツリー**に合わせ直す。
#
# これらのラッパーは musl の make install が configure 時の prefix を
# 直書きして生成する。**別の機械で作った ports/musl-install/ を持ち回ると
# 存在しない絶対パスが残り**、そこで作った動的リンクの実行ファイルは
#
#   [Requesting program interpreter: /home/itoh/MyOS/.../ld-musl-x86_64.so.1]
#
# という PT_INTERP を埋めてしまう (2026-09-12 に user/hello_dyn.elf で実測)。
#
# **ldso だけは作業ツリーではなく Orthox の中のパスにする。**PT_INTERP は
# 実行時にゲストが開く名前であって、ホストの場所ではない。rootfs には
# /lib/ld-musl-x86_64.so.1 を置いてある (Makefile の rootfs.img を参照)。
# ports/orthos-musl-gcc-dyn.sh が --dynamic-linker に渡しているのと同じ値。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="$ROOT/ports/musl-install"
GUEST_LDSO="/lib/ld-musl-x86_64.so.1"

[ -d "$SYSROOT" ] || { echo "no $SYSROOT" >&2; exit 1; }

for f in "$SYSROOT/bin/musl-clang" "$SYSROOT/bin/ld.musl-clang"; do
    [ -f "$f" ] || continue
    sed -i \
        -e "s|^libc=\".*\"$|libc=\"$SYSROOT\"|" \
        -e "s|^libc_inc=\".*\"$|libc_inc=\"$SYSROOT/include\"|" \
        -e "s|^libc_lib=\".*\"$|libc_lib=\"$SYSROOT/lib\"|" \
        -e "s|^ldso=\".*\"$|ldso=\"$GUEST_LDSO\"|" \
        "$f"
done

# ld-musl-x86_64.so.1 も別の機械の絶対パスを指す symlink になっていることが
# ある。同じディレクトリの libc.so を指す相対 symlink に貼り替える
LDSO="$SYSROOT/lib/ld-musl-x86_64.so.1"
if [ -L "$LDSO" ] && [ ! -e "$LDSO" ]; then
    ln -sf libc.so "$LDSO"
fi

echo "musl-clang paths -> $SYSROOT (ldso=$GUEST_LDSO)"
