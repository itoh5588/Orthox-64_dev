#!/bin/bash
# doomgeneric を aarch64-linux-musl の静的リンクで組む。
#
# **user/doomgeneric/doomgeneric/Makefile は使わない。** あちらは clang +
# x86 / newlib を前提にした分岐が積んであり、aarch64 の本物のクロス GCC
# (ports/orthos-aarch64-musl-gcc.sh) とは噛み合わない。**必要なのは
# 「全部の .c を 1 つのバイナリにする」だけ**なので、ここで直に組む。
#
# **プラットフォーム層は orthos のものだけを入れる。** doomgeneric_*.c は
# SDL / X11 / Windows など 9 つあり、全部入れると DG_Init が重複する。
#
#   scripts/build_doom_aarch64.sh <出力 ELF>
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/out/doomgeneric-aarch64.elf}"
SRC="$ROOT/user/doomgeneric/doomgeneric"
CC="${ORTHOS_AARCH64_CC:-$ROOT/ports/orthos-aarch64-musl-gcc.sh}"
BUILD="$ROOT/build/aarch64-doom"

[ -d "$SRC" ] || { echo "DOOM のソースが無い: $SRC" >&2; exit 1; }
[ -x "$CC" ]  || { echo "コンパイラが無い: $CC" >&2; exit 1; }

mkdir -p "$BUILD" "$(dirname "$OUT")"

# **ソースは Makefile の SRC_DOOM をそのまま使う。**
#
# 「*.c を全部」ではいけない。doomgeneric/ には SDL / X11 / Windows /
# allegro 向けの実装が同居していて、拾うと allegro/base.h が無いと言って
# 止まる (実測)。**除外リストで消す方式にすると、上流が増えたときに
# 黙って壊れる**ので、正の一覧を取る側にした。
#
# 一覧は user/doomgeneric/doomgeneric/Makefile の SRC_DOOM が持っている。
# **二重管理にしない** — あちらを直せばこちらも追随する
MKLIST=$(sed -n 's/^SRC_DOOM = //p' "$SRC/Makefile" | head -1)
[ -n "$MKLIST" ] || { echo "SRC_DOOM を Makefile から読めない" >&2; exit 1; }

SRCS=""
for o in $MKLIST; do
    c="$SRC/${o%.o}.c"
    [ -f "$c" ] || { echo "SRC_DOOM にあるが実体が無い: $c" >&2; exit 1; }
    SRCS="$SRCS $c"
done
SRCS="$SRCS $ROOT/user/orth_syscalls_aarch64.c"

# -DNORMALUNIX -DLINUX は doomgeneric が POSIX 経路を選ぶための既定。
# **-fcommon が要る。** doom は古い C で、複数の .c が同じ変数を extern
# 無しで宣言している (GCC 10 以降の既定 -fno-common では重複定義になる)
CFLAGS="-O2 -std=gnu99 -fcommon -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE"
# **音を入れる (D-3b)。** 付けていなかったので、実機の DOOM は無音だった。
#
#   FEATURE_SOUND  i_sound.c がこれを見て音のモジュールを繋ぐ。
#                  無いと sfx_module / music_module が空のままになる
#   ORTHOS         i_sound.c の SDL_mixer.h の取り込みを止める。
#                  **FEATURE_SOUND だけ付けると SDL を探しに行って落ちる**
#
# 出し先は i_orthossound.c (ミキサ込み) -> sound_pcm_u8 -> PWM の DMA。
# **カーネル側が積むだけで戻るようになった (D-3) のが前提。** 鳴らし切って
# から戻る版のままだと、512 サンプル @16kHz の提出ごとに 32ms 止まり、
# 35 tic/秒 の DOOM は 1 tic まるごと潰れる
CFLAGS="$CFLAGS -DFEATURE_SOUND -DORTHOS"
# **-I $ROOT/include を入れてはいけない。** そこにはカーネル側の stdio.h が
# あり、musl のものを隠して SEEK_END が消える (実測)。
# syscall.h は DOOM 側が相対パスで拾っているので -I は要らない
CFLAGS="$CFLAGS -I$SRC"
CFLAGS="$CFLAGS -Wno-implicit-function-declaration"

echo "=== doomgeneric (aarch64) を組む ==="
OBJS=""
for f in $SRCS; do
    o="$BUILD/$(basename "${f%.c}").o"
    "$CC" $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

# **静的リンク。** Orthox に動的リンカは載せていない
"$CC" -static -o "$OUT" $OBJS -lm
echo "=== できた ==="
ls -l "$OUT"
file "$OUT" 2>/dev/null || true
