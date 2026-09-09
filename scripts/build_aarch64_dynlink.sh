#!/bin/bash
# aarch64 の動的リンク試験一式を組む。**x86 の user/*.so と同じ顔ぶれ**を
# aarch64 で用意して、共有 musl (ports/musl-install-aarch64/lib/libc.so) が
# 実際に使えることを確かめるためのもの。
#
#   libdyn_a.so      __thread を持つ葉
#   libdyn_b.so      a を呼ぶ (.so 間の参照 + それぞれの TLS)
#   libdyn_plugin.so b を呼ぶ (3 段の連鎖。dlopen で開く)
#   libdyn_cpp.so    vtable と静的コンストラクタ (libstdc++ は使わない)
#   dynlink_multi_tls.elf  a と b の TLS が別々に効くか
#   dynlink_dlopen.elf     dlopen / dlsym
#   dynlink_malloc.elf     共有 libc の malloc
#
# **前提**: ports/musl-install-aarch64 が --enable-shared で組んであること
# (Makefile の $(AARCH64_MUSL_SYSROOT)/lib/libc.a)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SR=ports/musl-install-aarch64
OUT="${OUT:-out/aarch64-dyn}"
CC="${CC:-clang} --target=aarch64-linux-musl"
CXX="${CXX:-clang++} --target=aarch64-linux-musl"
LD="${LD:-ld.lld}"
INTERP=/lib/ld-musl-aarch64.so.1

[ -f "$SR/lib/libc.so" ] || {
  echo "★ $SR/lib/libc.so が無い。先に make $SR/lib/libc.a (--enable-shared)" >&2
  exit 1; }

mkdir -p "$OUT"
CFLAGS="-O2 -fPIC -nostdinc -isystem $SR/include -Wall"

# ---- 共有ライブラリ -------------------------------------------------------
# **-shared でも libc.so を繋ぐ。**TLS も malloc も libc 側に在る
# **-soname を必ず付ける。**付けないと DT_NEEDED に「リンク時に渡した
# パス」がそのまま入り (out/aarch64-dyn/libdyn_b.so のように)、実機では
# その経路が無いので "Error loading shared library" になる
mkso() { # mkso <出力> <ソース> [追加のライブラリ...]
  local out="$1" src="$2"; shift 2
  case "$src" in
    *.cc) $CXX $CFLAGS -fno-exceptions -fno-rtti -nostdinc++ -c "$src" -o "$out.o" ;;
    *)    $CC  $CFLAGS -c "$src" -o "$out.o" ;;
  esac
  $LD -shared -soname "$(basename "$out")" -o "$out" "$out.o" -L "$SR/lib" -lc "$@"
}

mkso "$OUT/libdyn_a.so"      user/dynlink_lib_a.c
mkso "$OUT/libdyn_b.so"      user/dynlink_lib_b.c      "$OUT/libdyn_a.so"
mkso "$OUT/libdyn_plugin.so" user/dynlink_plugin.c     "$OUT/libdyn_b.so" "$OUT/libdyn_a.so"
mkso "$OUT/libdyn_cpp.so"    user/dynlink_cpp_runtime.cc

# ---- 動的リンクした実行ファイル -------------------------------------------
mkelf() { # mkelf <出力> <ソース> [追加のライブラリ...]
  local out="$1" src="$2"; shift 2
  $CC $CFLAGS -fPIE -c "$src" -o "$out.o"
  $LD -pie --dynamic-linker="$INTERP" -o "$out" \
     "$SR/lib/Scrt1.o" "$SR/lib/crti.o" "$out.o" \
     -L "$SR/lib" "$@" -lc "$SR/lib/crtn.o"
}

mkelf "$OUT/hello_dyn.elf"         user/hello_dyn.c
mkelf "$OUT/dynlink_malloc.elf"    user/dynlink_malloc.c
mkelf "$OUT/dynlink_multi_tls.elf" user/dynlink_multi_tls.c "$OUT/libdyn_b.so" "$OUT/libdyn_a.so"
mkelf "$OUT/dynlink_dlopen.elf"    user/dynlink_dlopen.c

# ---- 配線証明 -------------------------------------------------------------
# **「組めた」と「動的リンクになっている」は別。**静的に落ちていないか見る
for f in "$OUT"/*.elf; do
  readelf -l -W "$f" | grep -q "program interpreter: $INTERP" || {
    echo "★ $f に PT_INTERP ($INTERP) が無い" >&2; exit 1; }
  readelf -d -W "$f" | grep -q "NEEDED.*libc.so" || {
    echo "★ $f が libc.so を要求していない" >&2; exit 1; }
done
for f in "$OUT"/*.so; do
  [ "$(readelf -h -W "$f" | awk '/Type:/{print $2}')" = "DYN" ] || {
    echo "★ $f が共有オブジェクトになっていない" >&2; exit 1; }
  readelf -d -W "$f" | grep -q "SONAME.*\[$(basename "$f")\]" || {
    echo "★ $f の SONAME が $(basename "$f") でない" >&2; exit 1; }
done
# **NEEDED に経路が混じっていないこと。**混じると実機で開けない
if readelf -d -W "$OUT"/*.elf "$OUT"/*.so | grep "NEEDED" | grep -q "/"; then
  echo "★ NEEDED に経路が入っている:" >&2
  readelf -d -W "$OUT"/*.elf "$OUT"/*.so | grep "NEEDED" | grep "/" >&2
  exit 1
fi
echo "  PT_INTERP と NEEDED  ok ($(ls "$OUT"/*.elf | wc -l) 本の elf / $(ls "$OUT"/*.so | wc -l) 本の so)"
ls -la "$OUT"/*.so "$OUT"/*.elf
