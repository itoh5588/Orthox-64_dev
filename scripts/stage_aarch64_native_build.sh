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
# **mkdir と rm も絶対パスで。** busybox の applet は /bin にあり、
# PATH の頭が /usr/bin なので、名前だけだと 1 コマンドごとに
# /usr/bin/mkdir への exec 失敗が 1 回入る。動きはするが、カーネルが
# 「Exec: File not found: /usr/bin/mkdir」を毎回出してログが汚れる
# (2026-08-23、47 本のビルドで 47 回出た)
MKDIR = /bin/mkdir
RM    = /bin/rm

# ---- どの機械向けに作るか --------------------------------------------------
#
# **2026-08-23 まで、ここには機械固有の指定が 1 つも無かった。**
# その結果ヘッダの既定値 (すべて QEMU virt) が効き、実機で作った
# カーネルは
#   - 0x40200000 にリンクされ (ファームウェアは 0x80000 に置いて飛ぶ)
#   - QEMU virt の UART 0x09000000 に喋る (実機の PL011 は 0xFE201000)
# ものになった。**読み込まれて起動し、1 行も出さずに死ぬ。**
#
# **8/11 に QEMU virt でセルフホストが通ったのは、既定値が全部 virt に
# 一致していたからで、この Makefile は暗黙に virt 専用だった。**
#
#   Raspberry Pi 4 : make                (既定。目的の機械)
#   QEMU virt      : make MACHINE=virt
#
# 値はホスト側 Makefile の aarch64-pi4-boot と同じものを写している。
# **片方だけ直すと、また「実機だけ黙る」に戻る。**
MACHINE ?= pi4

ifeq ($(MACHINE),pi4)
LOAD_PA     ?= 0x80000
MACHINE_DEFS = -DAARCH64_EARLY_UART=0xFE201000ULL \
	       -DAARCH64_CNTFRQ_HZ=54000000 \
	       -DAARCH64_EARLY_GICD=0xFF841000 \
	       -DAARCH64_EARLY_GICC=0xFF842000 \
	       -DAARCH64_SOUND=1 \
	       -DAARCH64_PCIE_BRCM_INIT=1 \
	       '-DAARCH64_INIT_PATH="/bin/ash"'
else
# QEMU virt はヘッダの既定でよい (8/11 まではこれで通っていた)
LOAD_PA     ?= 0x40200000
MACHINE_DEFS =
endif

CFLAGS = -std=gnu1x -mgeneral-regs-only -ffreestanding \
	 -fno-stack-protector -fno-stack-check -fno-lto -fno-pie \
	 -mcmodel=small -O2 -I$(SRCDIR)/include $(MACHINE_DEFS)

# **リンク番地は --defsym では渡せない。**
# GNU ld 2.42 は --defsym で定義した記号を、リンカスクリプトの
# DEFINED() の判定に反映しない (2026-08-23 に手元で再現)。lld は
# 反映するので、**ホストビルドだけが正しく効いていた。**
# 代入してから INCLUDE する 2 行のラッパーを作って回避する。
LDSCRIPT = $(BUILD)/machine.ld

LDFLAGS = -nostdlib -static -m aarch64elf -L $(SRCDIR)/scripts -T $(LDSCRIPT)

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

$(OUTPUT): $(OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	@echo "native-kernel-build-ok $@"

# **上の LDSCRIPT の注記で「2 行のラッパーを作る」と書いておきながら、
# 作る規則が無かった** (2026-08-27 に実機で露見)。
# BUILD を新しくすると必ず
#   ld: cannot open linker script file /kbuildN/machine.ld
# で落ちる。**実機に古い Makefile が載っている間は見えなかった。**
#
# INCLUDE の解決は LDFLAGS の -L $(SRCDIR)/scripts に任せる。
$(LDSCRIPT):
	@$(MKDIR) -p $(dir $@)
	@echo 'AARCH64_LOAD_PA = $(LOAD_PA);' > $@
	@echo 'INCLUDE kernel-aarch64.ld' >> $@

$(BUILD)/%.o: $(SRCDIR)/%.c
	@$(MKDIR) -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%_asm.o: $(SRCDIR)/%.S
	@$(MKDIR) -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) -rf $(BUILD) $(OUTPUT)

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

# **ラッパーを作る規則があること。** 無いと BUILD を新しくした初回に
# 必ずリンクで落ちる (2026-08-27 に実機で 30 分かけて露見させた)
grep -q '^\$(LDSCRIPT):' "$OUT/Makefile" || {
  echo "★ 生成した Makefile に \$(LDSCRIPT) を作る規則が無い" >&2; exit 1; }
echo "  リンカスクリプトのラッパーを作る規則がある"

# **実際に作らせて中身を見る。**規則が在るだけでは、中身が正しいか
# 分からない。makefile として評価させ、2 行そろうことを確かめる
_ldcheck="$(mktemp -d)"
if make --no-print-directory -C "$OUT" BUILD="$_ldcheck" "$_ldcheck/machine.ld" >/dev/null 2>&1 \
   && grep -q '^AARCH64_LOAD_PA = 0x' "$_ldcheck/machine.ld" \
   && grep -q '^INCLUDE kernel-aarch64.ld$' "$_ldcheck/machine.ld"; then
  echo "  ラッパーを実際に作らせて中身も確かめた"
else
  echo "★ ラッパーが作れないか中身が違う" >&2; rm -rf "$_ldcheck"; exit 1
fi
rm -rf "$_ldcheck"

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
