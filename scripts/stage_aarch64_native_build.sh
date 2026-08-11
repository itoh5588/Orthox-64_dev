#!/bin/bash
# Orthox aarch64 の中でカーネルを再ビルドするためのソースツリーを組み立てる。
# scripts/stage_riscv64_native_build.sh の aarch64 版。
#
#   出力: build/aarch64-native-src/  → rootfs の /src/kernel-build に入る
#
# Orthox 上での使い方:
#   cd /src/kernel-build && make
#
# riscv64 版との違い:
#   - **埋め込みブロブの objcopy が要らない。** riscv64 は
#     out/bootstrap-user-riscv64.elf を objcopy で .o にしていたが、
#     aarch64 は kernel/aarch64/user_blob.S が自己完結 (incbin 無し) で、
#     ユーザーコードを .S の中に直接書いている
#   - リンカスクリプトは scripts/kernel-aarch64.ld、-m aarch64elf
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/aarch64-native-src}"
# 途中で cd するので絶対パスにしておく
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

# ---- ホスト Makefile からソース一覧を取り出す ----------------------------
# **ハードコードしない。** ホスト側に 1 本足しただけでネイティブビルドが
# 落ちるようになるが、原因がイメージの中で出るので追うのが高くつく。
# 正規表現で読まず **make 自身に解釈させる** (行継続・変数展開を間違えない)
srcs_of() {  # $1 = 変数名
  make --no-print-directory -C "$ROOT" -f - print 2>/dev/null <<MK
include Makefile
print:
	@echo \$($1)
MK
}

AARCH64_C_SRCS="$(srcs_of AARCH64_C_SRCS)"
AARCH64_SHARED_C_SRCS="$(srcs_of AARCH64_SHARED_C_SRCS)"
AARCH64_ASM_SRCS="$(srcs_of AARCH64_ASM_SRCS)"

[ -n "$AARCH64_C_SRCS" ] || { echo "error: AARCH64_C_SRCS が取り出せない" >&2; exit 1; }
[ -n "$AARCH64_SHARED_C_SRCS" ] || { echo "error: AARCH64_SHARED_C_SRCS が取り出せない" >&2; exit 1; }
[ -n "$AARCH64_ASM_SRCS" ] || { echo "error: AARCH64_ASM_SRCS が取り出せない" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/kernel/aarch64" "$OUT/include" "$OUT/scripts"

# macOS の AppleDouble (._foo.c) は中身がバイナリなので必ず除外する。
# ソース配下に 900 個近くあり、混ざるとビルドが壊れる (riscv64 版と同じ)
copy_srcs() {  # $1=コピー元ディレクトリ $2=コピー先
  find "$1" -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' \) \
       ! -name '._*' -exec cp {} "$2/" \;
}

copy_srcs "$ROOT/kernel"          "$OUT/kernel"
copy_srcs "$ROOT/kernel/aarch64"  "$OUT/kernel/aarch64"

# include/ は arch 別サブディレクトリも要る
( cd "$ROOT/include" && find . -type f -name '*.h' ! -name '._*' -print0 ) \
  | ( cd "$ROOT/include" && xargs -0 -I{} cp --parents {} "$OUT/include/" )

cp "$ROOT/scripts/kernel-aarch64.ld" "$OUT/scripts/"

# ---- ネイティブ Makefile を書き出す --------------------------------------
# ホスト側の AARCH64_CFLAGS から **clang 専用のものだけ**を落とす:
#   --target=aarch64-none-elf   クロスの指定。ネイティブには要らない
#   -MMD -MP                    依存追跡。1 回きりのビルドには要らない
#   -Wall -Wextra               4.7.4 では別の警告が大量に出る (riscv64 版も落としている)
#   -fno-PIE -> -fno-pie        4.7.4 の綴り
# **-std=gnu1x を足す。** 4.7.4 に c11 の綴りは無く、渡さないと既定 C89 で
# 大量に落ちる (riscv64 版と同じ)。-mgeneral-regs-only と -mcmodel=small は
# 移植した 4.7.4 でも通ることを実機のアセンブリまで見て確認済み
{
cat <<'MAKEFILE_HEAD'
# Orthox aarch64 カーネル ネイティブビルド
# Orthox 上で実行:  cd /src/kernel-build && make
#
# **scripts/stage_aarch64_native_build.sh が生成する。手で編集しないこと。**
# ソース一覧はホスト側 Makefile から取り出しているので、ここを直しても
# 次の staging で上書きされる。ホスト側の Makefile を直すこと。

SRCDIR ?= /src/kernel-build
BUILD  ?= /kbuild
OUTPUT ?= /kernel-aarch64.elf

# 絶対パスで指定する。PATH 頭の /bin には無いので、名前だけだと
# 1 コマンドごとに /bin/gcc への exec 失敗が 1 回入る
# (動くが遅く、ログも汚れる。riscv64 版で踏んだ)
CC = /usr/bin/gcc
LD = /usr/bin/ld

CFLAGS = -std=gnu1x -mgeneral-regs-only -ffreestanding \
	 -fno-stack-protector -fno-stack-check -fno-lto -fno-pie \
	 -mcmodel=small -O2 -I$(SRCDIR)/include

LDFLAGS = -nostdlib -static -m aarch64elf -T $(SRCDIR)/scripts/kernel-aarch64.ld

MAKEFILE_HEAD

# 一覧はホスト Makefile から取ったものをそのまま書く
printf 'AARCH64_C_SRCS = \\\n'
for f in $AARCH64_C_SRCS;        do printf '\t%s \\\n' "$f"; done | sed '$ s/ \\$//'
printf '\nAARCH64_SHARED_C_SRCS = \\\n'
for f in $AARCH64_SHARED_C_SRCS; do printf '\t%s \\\n' "$f"; done | sed '$ s/ \\$//'
printf '\nASM_SRCS = \\\n'
for f in $AARCH64_ASM_SRCS;      do printf '\t%s \\\n' "$f"; done | sed '$ s/ \\$//'

cat <<'MAKEFILE_TAIL'

C_OBJS   = $(patsubst %.c,$(BUILD)/%.o,$(AARCH64_C_SRCS) $(AARCH64_SHARED_C_SRCS))
ASM_OBJS = $(patsubst %.S,$(BUILD)/%_asm.o,$(ASM_SRCS))
OBJS     = $(C_OBJS) $(ASM_OBJS)

all: $(OUTPUT)

$(OUTPUT): $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo "native-kernel-build-ok $@"

$(BUILD)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD) $(OUTPUT)

.PHONY: all clean
MAKEFILE_TAIL
} > "$OUT/Makefile"

# ---- 配線証明 -------------------------------------------------------------
# **staging はイメージに入れて OS を起動するまで結果が出ない。**
# 中で「そんなファイルは無い」で落ちると、原因を追うのに QEMU 1 往復かかる。
# ここで潰せるものは全部ここで潰す。

missing=""
for f in $AARCH64_C_SRCS $AARCH64_SHARED_C_SRCS $AARCH64_ASM_SRCS; do
  [ -f "$OUT/$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || { echo "★ staging に入っていないソース:$missing" >&2; exit 1; }
echo "  ソース $(echo $AARCH64_C_SRCS $AARCH64_SHARED_C_SRCS $AARCH64_ASM_SRCS | wc -w) 本が全部揃っている"

[ -f "$OUT/scripts/kernel-aarch64.ld" ] || {
  echo "★ リンカスクリプトが入っていない" >&2; exit 1; }

# **ヘッダの取りこぼしを実測で見る。** #include "..." の相対取り込みを
# 全部辿るのは高いので、include/ 直下の .h の本数がホストと一致することを見る
h_host="$(cd "$ROOT/include" && find . -type f -name '*.h' ! -name '._*' | wc -l)"
h_out="$(cd "$OUT/include" && find . -type f -name '*.h' | wc -l)"
[ "$h_host" = "$h_out" ] || {
  echo "★ include/ の .h が host=$h_host staged=$h_out で一致しない" >&2; exit 1; }
echo "  include/ の .h $h_out 本がホストと一致"

# 生成した Makefile が make として読めること (構文と一覧の書き出しの検算)
n_obj="$(make --no-print-directory -C "$OUT" -f Makefile print-objs 2>/dev/null \
         --eval 'print-objs:;@echo $(words $(OBJS))' || echo 0)"
[ "$n_obj" = "$(echo $AARCH64_C_SRCS $AARCH64_SHARED_C_SRCS $AARCH64_ASM_SRCS | wc -w)" ] || {
  echo "★ 生成した Makefile の OBJS が $n_obj 個で一覧と合わない" >&2; exit 1; }
echo "  生成した Makefile の OBJS $n_obj 個が一覧と一致"

echo
echo "組み立て完了: $OUT"
du -sh "$OUT"
find "$OUT" -name '*.c' | wc -l | sed 's/^/  .c ファイル: /'
find "$OUT" -name '*.h' | wc -l | sed 's/^/  .h ファイル: /'
find "$OUT" -name '*.S' | wc -l | sed 's/^/  .S ファイル: /'
