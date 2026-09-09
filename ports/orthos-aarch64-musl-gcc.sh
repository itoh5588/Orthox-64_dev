#!/bin/bash
# aarch64-linux-musl 静的リンク用コンパイラドライバ。
# ports/orthos-riscv64-musl-gcc.sh の aarch64 版だが、**中身の方針が違う。**
#
#   riscv64  clang + sysroot を手で指定し、crt1/crti/libc/libm/libgcc/crtn を
#            自分で並べる (clang は musl の sysroot を知らないため)
#   aarch64  **移植した本物のクロス GCC を使う。**
#            --with-sysroot=ports/musl-install-aarch64 で組んであるので、
#            crt も libc も libgcc も gcc が自分で解決できる。
#            こちらがやるのは「じゃまな指定を落とす」ことだけ
#
# なぜドライバが要るか:
#   ports/build_busybox_ash.sh が LDFLAGS に **-nostartfiles を固定で**渡す。
#   riscv64 は crt を自前で並べるのでそれが正しいが、gcc に任せる構成では
#   crt1.o が落ちて _start が無くなる。ここで落とす。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GCC="${ORTHOS_AARCH64_GCC:-$ROOT/ports/cross-aarch64/bin/aarch64-linux-musl-gcc}"

[ -x "$GCC" ] || {
  echo "error: $GCC が無い。先に ports/build_gcc474_aarch64_stage2.sh" >&2; exit 1; }

# **段を見分ける。** riscv64 のドライバと同じ 3 分け。
# ここを落として busybox の部分リンクを壊した:
#   applets/built-in.o は -r (再配置リンク) で作られるが、そこで crt を足すと
#   _start / _init / _fini / __dso_handle が中に入り、最終リンクで
#   crt1.o と multiple definition になる
compile_only=false
reloc_link=false
for arg in "$@"; do
    case "$arg" in
        -c|-E|-S)     compile_only=true ;;
        -r|-Wl,-r)    reloc_link=true ;;
    esac
done

args=()
for arg in "$@"; do
    case "$arg" in
        # **最終リンクのときだけ落とす。** gcc に crt と libc を解決させるため。
        # 部分リンクでは -nostdlib が正しいので残す
        -nostartfiles|-nostdlib)
            if $compile_only || $reloc_link; then args+=("$arg"); fi
            continue ;;
        # 明示的に並べられた crt は gcc のものと二重になる
        */crt0.o|*/crt1.o|*/crti.o|*/crtn.o|*/rcrt1.o) continue ;;
        # gcc が自分で付ける
        -lc|-lgcc|-lgcc_s|*/libgcc_s.so*) continue ;;
    esac
    args+=("$arg")
done

# -D__ORTHOS__ は busybox の Orthox 向け分岐に要る (riscv64 と同じ)。
# -fno-PIE / -static は Orthox に動的リンカが無いため。
cmd=("$GCC" -fno-PIE -fno-pie -D__ORTHOS__)
if $reloc_link; then
    # 部分リンク。**-static も付けない** (意味が無いうえ警告が出る)
    cmd+=(-nostdlib)
else
    cmd+=(-static)
fi
cmd+=("${args[@]}")

exec "${cmd[@]}"
