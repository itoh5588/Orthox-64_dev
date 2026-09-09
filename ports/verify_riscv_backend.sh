#!/bin/bash
# 組み上がった GCC 4.6.4 (RISC-V フォーク) が Orthox の要求を満たすか検証する。
#
# 見るのは 6 点:
#   1. lp64d      … double が fa0..fa7 (浮動小数レジスタ) で渡ること
#   2. medany     … 実際に R_RISCV_PCREL_HI20 のリロケーションになること
#   3. rv64gc     … 圧縮命令 (RVC) を実際に生成すること
#   4. long double… __divtf3 に落ちること (musl の printf が踏む)
#   5. TLS        … __thread が native TLS になること (emutls では musl に使えない)
#   6. __atomic_* … 4.6.4 に無いこと / __sync_* が使えること
#
# アセンブル・逆アセンブルまで行うので ./build_binutils_riscv64.sh が先に要る。
set -u

PORTS="$(cd "$(dirname "$0")" && pwd)"
# 第 1 引数で対象ツリーを選ぶ。既定は 4.7.4 移植版 (本命)。
#   ./verify_riscv_backend.sh                  → gcc-4.7.4-riscv
#   ./verify_riscv_backend.sh gcc-4.6.4-riscv  → 移植前のフォーク
TREE="${1:-gcc-4.7.4-riscv}"
BDIR="$PORTS/$TREE/build-riscv64/gcc"
XGCC="$BDIR/xgcc"
PREFIX="$PORTS/cross-riscv64"
AS="$PREFIX/bin/riscv64-linux-musl-as"
OBJDUMP="$PREFIX/bin/riscv64-linux-musl-objdump"
READELF="$PREFIX/bin/riscv64-linux-musl-readelf"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for t in "$XGCC" "$AS" "$OBJDUMP" "$READELF"; do
  [ -x "$t" ] || { echo "error: $t が無い。build_binutils_riscv64.sh と build_gcc464_riscv.sh を先に通すこと"; exit 1; }
done

# 既定は Orthox が使う組み合わせ。
# 「わざと壊すと落ちる」ことの確認用に差し替えられるようにしてある:
#   ORTHOS_VERIFY_ARCH="-march=rv64g -mabi=lp64"  → 1 と 3 が BAD になるはず
ARCH="${ORTHOS_VERIFY_ARCH:--march=rv64gc -mabi=lp64d}"

pass=0; fail=0
ok()  { echo "  OK   $1"; pass=$((pass+1)); }
bad() { echo "  BAD  $1"; fail=$((fail+1)); }

# $1=名前 $2=ソース本体 $3…=追加フラグ  → $WORK/$1.s と $WORK/$1.o を作る
build() {
  local n="$1" src="$2"; shift 2
  printf '%s\n' "$src" > "$WORK/$n.c"
  "$XGCC" -B"$BDIR" -S -O2 $ARCH "$@" "$WORK/$n.c" -o "$WORK/$n.s" 2>"$WORK/$n.err" || return 1
  "$AS" $ARCH "$WORK/$n.s" -o "$WORK/$n.o" 2>>"$WORK/$n.err" || return 1
}

echo "=== 使用するコンパイラ / アセンブラ ==="
"$XGCC" -B"$BDIR" --version | head -1
"$XGCC" -B"$BDIR" -dumpmachine
"$AS" --version | head -1
echo

# --- 1. lp64d ------------------------------------------------------------
echo "=== 1. lp64d (double の引数渡し) ==="
if build f 'double addd(double a, double b) { return a + b; }'; then
  grep -E "fadd|fa[0-9]" "$WORK/f.s" | head -3 | sed 's/^/    /'
  grep -q "fadd\.d"        "$WORK/f.s" && ok "fadd.d を生成 (ハード浮動小数)" || bad "soft-float に落ちている"
  grep -qE "\bfa[01]\b"    "$WORK/f.s" && ok "引数が fa0/fa1 (lp64d の呼出規約)" || bad "fa0/fa1 が無い"
else
  bad "コンパイル失敗: $(head -3 "$WORK/f.err")"
fi
echo

# --- 2. medany のリロケーション (実物で見る) ------------------------------
echo "=== 2. -mcmodel=medany のリロケーション ==="
# アセンブリ上は 'lw a0,g' の擬似命令形式なので、.o のリロケーションで判定する
if build g 'extern int g; int readg(void) { return g; }' -mcmodel=medany; then
  # -W を付けないと readelf がリロケーション名を桁で切る (R_RISCV_PCREL_HI2 になる)
  "$READELF" -rW "$WORK/g.o" | grep -E "R_RISCV" | head -4 | sed 's/^/    /'
  "$READELF" -rW "$WORK/g.o" | grep -q "R_RISCV_PCREL_HI20" \
    && ok "R_RISCV_PCREL_HI20 (批准後の仕様・位置独立)" || bad "PCREL_HI20 が出ない"
  "$READELF" -rW "$WORK/g.o" | grep -q "R_RISCV_PCREL_LO12" \
    && ok "R_RISCV_PCREL_LO12" || bad "PCREL_LO12 が出ない"
else
  bad "コンパイル失敗: $(head -3 "$WORK/g.err")"
fi
# medlow 側は %hi/%lo が明示的に出る
if build gl 'extern int g; int readg(void) { return g; }'; then
  grep -q "%hi(" "$WORK/gl.s" && ok "medlow は %hi/%lo を明示 (絶対アドレス)" || bad "medlow で %hi が無い"
fi
echo

# --- 3. RVC (圧縮命令) ---------------------------------------------------
echo "=== 3. rv64gc の圧縮命令 (RVC) ==="
if [ -f "$WORK/f.o" ]; then
  # -M no-aliases が要る。既定の objdump は c.jr を 'ret' のようにエイリアス名で出すので
  # 'c.' を探しても引っ掛からない (命令長 2 バイトなのに見落とす)
  "$OBJDUMP" -d -M no-aliases "$WORK/f.o" | grep -E "\bc\.[a-z]+" | head -3 | sed 's/^/    /'
  "$OBJDUMP" -d -M no-aliases "$WORK/f.o" | grep -qE "\bc\.[a-z]+" \
    && ok "圧縮命令を生成 (rv64gc の C 拡張)" || bad "圧縮命令が 1 つも無い"
  "$READELF" -A "$WORK/f.o" | grep -q "Tag_RISCV_arch.*_c[0-9]" \
    && ok "Tag_RISCV_arch に C 拡張" || bad "ELF 属性に C 拡張が無い"
fi
echo

# --- 4. long double ------------------------------------------------------
echo "=== 4. long double (musl の printf が踏む __divtf3) ==="
if build ld 'long double divld(long double a, long double b) { return a / b; }'; then
  grep -E "call|__div" "$WORK/ld.s" | head -2 | sed 's/^/    /'
  grep -q "__divtf3" "$WORK/ld.s" && ok "__divtf3 を呼ぶ (128bit long double)" \
                                  || bad "__divtf3 が出ない = long double が 128bit でない"
else
  bad "コンパイル失敗: $(head -3 "$WORK/ld.err")"
fi
echo

# --- 5. TLS --------------------------------------------------------------
# ここが emutls だと musl (native TLS 前提) には使えない。
# アセンブラ抜きで configure すると gcc_cv_as='' → HAVE_AS_TLS undef → emutls に落ちる。
echo "=== 5. TLS (__thread) ==="
if build t '__thread int tv; int readt(void) { return tv; }'; then
  grep -E "tprel|emutls|\btp\b" "$WORK/t.s" | head -3 | sed 's/^/    /'
  if grep -q "__emutls" "$WORK/t.s"; then
    bad "emutls に落ちている (HAVE_AS_TLS が undef。musl には使えない)"
  elif grep -qE "%tprel_hi|\btp\b" "$WORK/t.s"; then
    ok "native TLS (tp 相対)"
  else
    bad "TLS の形が判別できない"
  fi
else
  bad "コンパイル失敗: $(head -3 "$WORK/t.err")"
fi
echo

# --- 6. __atomic_* / __sync_* --------------------------------------------
echo "=== 6. __atomic_* / __sync_* の対応 ==="
# 4.6.4 には __atomic_* 組み込み関数が無く、4.7.4 にはある。
# backend の sync.md は 4.7 以降の optab 名 (atomic_*) で書かれているので、
# 4.7.4 では組み込み関数が通るうえに inline 展開されるのが期待値。
if build at 'int v; int f(void) { return __atomic_fetch_add(&v, 1, __ATOMIC_SEQ_CST); }' 2>/dev/null; then
  if "$OBJDUMP" -d "$WORK/at.o" | grep -qE "\bamo(add|swap)|\blr\.|\bsc\."; then
    "$OBJDUMP" -d "$WORK/at.o" | grep -E "amo|lr\.|sc\." | head -1 | sed 's/^/    /'
    ok "__atomic_fetch_add がインライン展開される (4.7 以降の期待値)"
  else
    bad "__atomic_fetch_add が通るが libcall になる"
  fi
else
  case "$TREE" in
    *4.6.4*) ok "__atomic_fetch_add は未対応 (4.6.4 の期待値。組み込み関数が無い)" ;;
    *)       bad "__atomic_fetch_add が使えない (4.7 なら使えるはず)" ;;
  esac
fi
if build sy 'int v; int f(void) { return __sync_fetch_and_add(&v, 1); }'; then
  # コンパイルが通るだけでは不十分。インライン展開されず libcall になると
  # -nostdlib かつ -lgcc 無しのカーネルではリンクできない。
  if "$OBJDUMP" -d "$WORK/sy.o" | grep -qE "\bamo(add|swap)|\blr\.|\bsc\."; then
    "$OBJDUMP" -d "$WORK/sy.o" | grep -E "amo|lr\.|sc\." | head -2 | sed 's/^/    /'
    ok "__sync_fetch_and_add がインライン展開される"
  else
    "$READELF" -sW "$WORK/sy.o" | grep -oE "__sync_[a-z_0-9]+" | head -2 | sed 's/^/    未定義: /'
    bad "__sync_* が libcall になる (backend の sync.md は 4.7+ のパターン名なので 4.6.4 では効かない)"
  fi
else
  bad "__sync_fetch_and_add も未対応: $(head -3 "$WORK/sy.err")"
fi
echo

echo "=== 結果: OK $pass / BAD $fail ==="
[ "$fail" -eq 0 ]
