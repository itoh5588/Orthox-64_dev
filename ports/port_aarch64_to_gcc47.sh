#!/bin/bash
# GCC 4.9.4 の AArch64 バックエンドを GCC 4.7.4 へ **後方移植**する。
# ports/port_riscv_to_gcc47.sh の aarch64 版。
#
# なぜ 4.7.4 か:
#   - 4.7.4 は「C だけでビルドできる最後の GCC」(gcc/Makefile.in が COMPILER=$(CC))。
#     4.8 以降は COMPILER=$(CXX) で、Orthox 上で動く cc1 を作るには
#     **aarch64-linux-musl 向けの libstdc++** が要る。4.7.4 ならそれが要らない
#   - x86 / riscv64 が既に 4.7.4 なので版が揃う
#
# なぜ移植が要るか:
#   4.7.4 に aarch64 は無い (config/ にあるのは 32bit の arm のみ)。
#   上流入りは 4.8。**riscv は前方移植 (4.6→4.7) だったが、これは後方移植。**
#
# 移植量 (実測):
#   gcc/config/aarch64          48,004 行。うち arm_neon.h が 25,391 行
#   libgcc/config/aarch64          588 行 / 9 ファイル
#   4.7.4 に無いフック               4 個 (うち 3 個はベクトル化・NEON 由来)
#
# 既存の ports/gcc-4.7.4 (x86 用) と ports/gcc-4.7.4-riscv は触らない。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
DST="$PORTS/gcc-4.7.4-aarch64"
TARBALL47="$PORTS/gcc-4.7.4.tar.gz"
TARBALL49="$PORTS/gcc-4.9.4.tar.gz"
STAGE="$PORTS/.gcc494-aarch64-parts"

[ -f "$TARBALL47" ] || { echo "error: $TARBALL47 が無い" >&2; exit 1; }
[ -f "$TARBALL49" ] || { echo "error: $TARBALL49 が無い" >&2; exit 1; }

# --- 4.7.4 を展開 ---------------------------------------------------------
if [ ! -d "$DST" ]; then
  echo "--- gcc-4.7.4 を $DST に展開"
  mkdir -p "$DST"
  tar -xzf "$TARBALL47" -C "$DST" --strip-components=1
fi

# --- 4.9.4 から要る部分だけ取り出す ---------------------------------------
# 111MB の tar.gz を毎回走査すると 2〜3 分かかるので、一度出したら使い回す
# **判定はディレクトリではなくファイルで行う。** 以前の版が arm/ に 3 個だけ
# 置いていたため、ディレクトリ判定だと「取得済み」と誤認して取り直さなかった
if [ ! -f "$STAGE/gcc-4.9.4/gcc/config/arm/types.md" ]; then
  echo "--- gcc-4.9.4 から aarch64 関連を取り出す (2〜3 分)"
  mkdir -p "$STAGE"
  # **config/arm は丸ごと取る。** aarch64.md が arm 側の .md を推移的に
  # 取り込んでおり、必要な本数を先に知ることができない (下で閉包を計算する)
  tar -xzf "$TARBALL49" -C "$STAGE" \
      gcc-4.9.4/gcc/config/aarch64 \
      gcc-4.9.4/gcc/common/config/aarch64 \
      gcc-4.9.4/libgcc/config/aarch64 \
      gcc-4.9.4/gcc/config/arm \
      gcc-4.9.4/gcc/config.gcc \
      gcc-4.9.4/gcc/configure \
      gcc-4.9.4/libgcc/config.host
fi
SRC="$STAGE/gcc-4.9.4"

# **無いものを写そうとして後で気づくのを防ぐ**
for f in gcc/config/aarch64/aarch64.c gcc/config/aarch64/biarchlp64.h \
         gcc/config/arm/aarch-common.c gcc/common/config/aarch64/aarch64-common.c; do
  [ -f "$SRC/$f" ] || { echo "error: $SRC/$f が取り出せていない" >&2; exit 1; }
done

# --- バックエンド本体をそのまま持ち込む -----------------------------------
echo "--- バックエンドを複製"
rm -rf "$DST/gcc/config/aarch64" "$DST/libgcc/config/aarch64" \
       "$DST/gcc/common/config/aarch64"
cp -a "$SRC/gcc/config/aarch64"        "$DST/gcc/config/aarch64"
cp -a "$SRC/libgcc/config/aarch64"     "$DST/libgcc/config/aarch64"
cp -a "$SRC/gcc/common/config/aarch64" "$DST/gcc/common/config/aarch64"

# aarch-common.* は aarch64 が入ったとき (4.8) に arm と共通化するため新設された。
# **4.7.4 には無いので一緒に持ち込む。** config.gcc が extra_objs で要求する
echo "--- config/arm/aarch-common.* を持ち込む (4.7.4 に無い)"
for f in aarch-common.c aarch-common-protos.h aarch-cost-tables.h; do
  cp "$SRC/gcc/config/arm/$f" "$DST/gcc/config/arm/$f"
done

# --- aarch64.md が取り込む arm 側の .md を推移的に持ち込む -----------------
# **1 本ずつ追いかけて 2 回やり直した。** aarch64.md -> arm/cortex-a15.md ->
# arm/cortex-a15-neon.md と 2 段以上あり、欠けると genmddeps が
# 「include file `../arm/types.md' not found」で止まる。
#
# **4.7.4 版が既にあるものも 4.9.4 版で上書きする。** 命令種別の名前体系が
# 変わっており (4.7.4: alu / alu_shift、4.9.4: udiv / sdiv / block)、
# 混ぜると eq_attr "type" が解決しない。このツリーは aarch64 専用なので
# arm バックエンドを壊しても構わない
echo "--- aarch64.md が取り込む arm/*.md を推移的に複製"
python3 - "$SRC/gcc" "$DST/gcc" <<'PY'
import os, re, sys, shutil
src, dst = sys.argv[1], sys.argv[2]
inc = re.compile(r'\(include\s+"([^"]+)"')

def closure(root, start):
    seen, todo = set(), [start]
    while todo:
        f = todo.pop()
        if f in seen:
            continue
        seen.add(f)
        p = os.path.join(root, f)
        if not os.path.exists(p):
            continue
        for m in inc.findall(open(p, encoding='utf-8', errors='replace').read()):
            todo.append(os.path.normpath(os.path.join(os.path.dirname(f), m)))
    return seen

need = [f for f in sorted(closure(src, 'config/aarch64/aarch64.md'))
        if f.startswith('config/arm/')]
for f in need:
    shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
print("    arm/*.md を %d 本複製 (%s)"
      % (len(need), ', '.join(os.path.basename(f) for f in need)))

# **配線証明: 複製後に全 include が解決するか確かめる。**
# ここで気づかないと genmddeps まで進んでから落ちる
missing = [f for f in closure(dst, 'config/aarch64/aarch64.md')
           if not os.path.exists(os.path.join(dst, f))]
if missing:
    raise SystemExit("★ include が解決しない: %s" % ', '.join(missing))
print("    include の解決を確認 (未解決 0 件)")
PY

# --- ビルド設定へのハンク -------------------------------------------------
python3 - "$SRC" "$DST" <<'PY'
import sys, io, os
src, dst = sys.argv[1], sys.argv[2]

def lines(p):
    with io.open(p, encoding='utf-8', errors='surrogateescape') as f:
        return f.readlines()

def write(p, ls):
    with io.open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(ls)

def find(ls, needle, start=0):
    for i in range(start, len(ls)):
        if ls[i].rstrip('\n') == needle:
            return i
    raise SystemExit("見つからない: %r" % needle)

def cut(ls, begin, end_exclusive, start=0):
    b = find(ls, begin, start)
    e = find(ls, end_exclusive, b + 1)
    return ls[b:e]

def find_nth(ls, needle, n):
    """n 番目 (1 始まり) の出現位置。**同じ行が複数ある目印に使う**"""
    seen = 0
    for i, l in enumerate(ls):
        if l.rstrip('\n') == needle:
            seen += 1
            if seen == n:
                return i
    raise SystemExit("%d 番目が見つからない: %r" % (n, needle))

# ---- gcc/config.gcc : 4 ブロック ----
# **2 つで足りると思って 2 回やり直した。** config.gcc は目的の違う case が
# 4 つあり、どれか 1 つでも欠けると configure が下記で落ちる:
#   supported_defaults 欠     -> "This target does not support --with-abi."
#   target_cpu_default2 欠    -> --with-cpu が既定値に反映されない (黙って無視)
s_cfg = lines(os.path.join(src, 'gcc/config.gcc'))
blk_cpu   = cut(s_cfg, 'aarch64*-*-*)',      'alpha*-*-*)')
blk_linux = cut(s_cfg, 'aarch64*-*-linux*)', 'alpha*-*-linux*)')
# こちらの 2 つはタブ字下げ (case の中身が 1 段深い)
blk_supp  = cut(s_cfg, '\taarch64*-*-*)', '\talpha*-*-*)')
blk_cpud  = cut(s_cfg, '\taarch64*-*-*)', '\tarm*-*-*)',
                find(s_cfg, '\taarch64*-*-*)') + 1)

# **ABI 選択を target ブロックの先頭へ畳み込む。**
# 4.9.4 は独立した case で tm_p_file と biarch の tm_file を先に決めているが、
# Orthox は LP64 のみなので分岐は要らない。tm_file の前置順序は保つ
# (biarchlp64.h が aarch64/aarch64.h より前に来ること)
fold = ['\ttm_p_file="${tm_p_file} arm/aarch-common-protos.h"\n',
        '\ttm_file="aarch64/biarchlp64.h ${tm_file}"\n']
blk_linux = blk_linux[:1] + fold + blk_linux[1:]

d_cfg_p = os.path.join(dst, 'gcc/config.gcc')
d_cfg = lines(d_cfg_p)
done = []

# **ファイルの後ろにあるものから挿す。** 前に挿すと後ろの行番号がずれる。
# 判定はブロックごとに独立させる (一部だけ入った状態からでも直せるように)
if 'target_cpu_default2=$target_cpu_cname' not in ''.join(d_cfg):
    i = find_nth(d_cfg, '\talpha*-*-*)', 2)       # target_cpu_default2 の case
    d_cfg = d_cfg[:i] + blk_cpud + d_cfg[i:]
    done.append('target_cpu_default2')
if 'supported_defaults="abi cpu arch"' not in ''.join(d_cfg):
    i = find_nth(d_cfg, '\talpha*-*-*)', 1)       # supported_defaults の case
    d_cfg = d_cfg[:i] + blk_supp + d_cfg[i:]
    done.append('supported_defaults')
if 'aarch64*-*-linux*)' not in ''.join(d_cfg):
    i = find(d_cfg, 'alpha*-*-linux*)')
    d_cfg = d_cfg[:i] + blk_linux + d_cfg[i:]
    done.append('aarch64-linux')
if 'cpu_type=aarch64' not in ''.join(d_cfg):
    i = find(d_cfg, 'alpha*-*-*)')
    d_cfg = d_cfg[:i] + blk_cpu + d_cfg[i:]
    done.append('cpu_type')

if done:
    write(d_cfg_p, d_cfg)
    print("    config.gcc: %s を挿入" % ' / '.join(reversed(done)))
else:
    print("    config.gcc: 適用済み。skip")

# ---- libgcc/config.host : cpu_type と tmake の 2 ブロック ----
s_host = lines(os.path.join(src, 'libgcc/config.host'))
blk_hcpu = cut(s_host, 'aarch64*-*-*)',      'alpha*-*-*)')
blk_hlin = cut(s_host, 'aarch64*-*-linux*)', 'alpha*-*-linux*)')

d_host_p = os.path.join(dst, 'libgcc/config.host')
d_host = lines(d_host_p)
if 'cpu_type=aarch64' in ''.join(d_host):
    print("    config.host: 適用済み。skip")
else:
    i2 = find(d_host, 'alpha*-*-linux*)')
    d_host = d_host[:i2] + blk_hlin + d_host[i2:]
    i1 = find(d_host, 'alpha*-*-*)')
    d_host = d_host[:i1] + blk_hcpu + d_host[i1:]
    write(d_host_p, d_host)
    print("    config.host: cpu_type / tmake の 2 ブロックを挿入")

# ---- config.sub : CPU 一覧 と musl 対応 ----
# 4.7.4 の config.sub は aarch64 も musl も知らない (実測)。
# musl を知らないと aarch64-linux-musl が basic_machine='aarch64-linux' と
# 誤分割され「machine `aarch64-linux' not recognized」で configure が落ちる
p = os.path.join(dst, 'config.sub')
ls = lines(p)
changed = []

if not any('aarch64' in l for l in ls):
    i = find(ls, '\t| we32k \\')
    ls = ls[:i] + ['\t| aarch64 | aarch64_be \\\n'] + ls[i:]
    changed.append('aarch64 / aarch64_be を CPU 一覧に追加')

if not any('musl' in l for l in ls):
    i = find(ls, '  linux-uclibc* | uclinux-uclibc* | uclinux-gnu* | kfreebsd*-gnu* | \\')
    ls[i] = '  linux-musl* | ' + ls[i].lstrip()
    j = find(ls, '\t      | -linux-newlib* | -linux-uclibc* \\')
    ls[j] = '\t      | -linux-newlib* | -linux-musl* | -linux-uclibc* \\\n'
    changed.append('musl を OS 一覧に追加 (2 箇所)')

if changed:
    write(p, ls)
    for c in changed:
        print("    config.sub: %s" % c)
else:
    print("    config.sub: 適用済み。skip")

# ---- gcc/configure(.ac) : TLS 検査と cpu_type 一覧 ----
# TLS の case が無いと検査自体が走らず HAVE_AS_TLS が undef のままになり、
# __thread が emutls に落ちる。musl は native TLS 前提なので致命的。
# **ビルドは成功してしまい生成コードだけが変わるので気づきにくい**
s_conf = lines(os.path.join(src, 'gcc/configure'))
b = find(s_conf, '  aarch64*-*-*)')
e = find(s_conf, '\t;;', b + 1)
tls_blk = s_conf[b:e + 1]

for name in ('configure', 'configure.ac'):
    p2 = os.path.join(dst, 'gcc', name)
    cc = lines(p2)
    if any('aarch64' in l for l in cc):
        print("    gcc/%s: 適用済み。skip" % name)
        continue
    i = find(cc, '  alpha*-*-*)')
    cc[i:i] = tls_blk
    # アセンブラ検査で nop を出せる cpu_type の一覧にも aarch64 を足す
    old = '  alpha | arm | avr | bfin | cris | i386 | m32c | m68k | microblaze | mips \\\n'
    new = '  aarch64 | alpha | arm | avr | bfin | cris | i386 | m32c | m68k | microblaze | mips \\\n'
    n2 = sum(1 for l in cc if l == old)
    cc = [new if l == old else l for l in cc]
    write(p2, cc)
    print("    gcc/%s: TLS 検査の aarch64 case を追加 / cpu_type 一覧 (%d 箇所)"
          % (name, n2))

# ---- aarch64.c : 4.7.4 に無いフックの登録を落とす ----
# 実測で 68 個中 4 個。うち 3 個はベクトル化・NEON 由来でカーネルには要らない。
#   TARGET_LRA_P                     LRA は 4.8 で新設。4.7 の reload を使う
#   TARGET_GIMPLE_FOLD_BUILTIN       NEON 組み込み関数の畳み込み
#   TARGET_VECTORIZE_ADD_STMT_COST   4.8 のベクトル化コストモデル
#   TARGET_VECTORIZE_BUILTINS        NEON 組み込み関数の存在標識
# **関数本体は残す。** 未使用警告が出るだけで、消すと依存関係を追う手間が増える
DROP = ('TARGET_LRA_P', 'TARGET_GIMPLE_FOLD_BUILTIN',
        'TARGET_VECTORIZE_ADD_STMT_COST', 'TARGET_VECTORIZE_BUILTINS')
p3 = os.path.join(dst, 'gcc/config/aarch64/aarch64.c')
ls = lines(p3)
out, i, removed = [], 0, []
while i < len(ls):
    s = ls[i].rstrip('\n')
    hit = next((h for h in DROP if s == '#undef ' + h), None)
    if hit:
        i += 1                                  # #undef を飛ばす
        while i < len(ls) and ls[i].startswith('#define ' + hit):
            while ls[i].rstrip('\n').endswith('\\'):   # 継続行も飛ばす
                i += 1
            i += 1
        removed.append(hit)
        continue
    out.append(ls[i]); i += 1
if removed:
    write(p3, out)
    print("    aarch64.c: フック登録 %d 組を削除 (%s)" % (len(removed), ', '.join(removed)))
else:
    print("    aarch64.c: 適用済み。skip")
PY

# --- NEON を落とす --------------------------------------------------------
# **4.7.4 は define_int_iterator を持たない。** これは 4.8 で機械記述リーダが
# 「iterator 番号を実体番号の先に割り当てる」方式から「置換すべき位置を記録
# する」方式へ作り替えられたときに入った機能で、int は値空間に上限が無いため
# 前者の方式では成立しない。**リーダの再設計ごと移植するのは中核 (全 gen* が
# 依存) に手を入れることになる。**
#
# 実測した使用状況:
#   aarch64-simd.md   int_iterator 40 / int_attr 88   <- NEON
#   aarch64.md        int_iterator  2 / int_attr  4   <- frint / lfcvt だけ
#
# 本体側の 6 箇所は __builtin_trunc / lround 系の FP 丸め命令で、カーネルは
# -mgeneral-regs-only かつ freestanding なので使わない。**スカラ FP は
# aarch64.md に mode iterator で書かれており無傷** (add/mul/div/sqrt)。
# よって NEON を落とす。できる cc1 は NEON 非対応になる
echo "--- NEON を落とす (4.7.4 に define_int_iterator が無いため)"
python3 - "$DST/gcc" <<'PY'
import io, os, re, sys
g = sys.argv[1]
A = os.path.join(g, 'config/aarch64')

def lines(p):
    with io.open(p, encoding='utf-8', errors='surrogateescape') as f:
        return f.readlines()

def write(p, ls):
    with io.open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(ls)

def read_text(p):
    with io.open(p, encoding='utf-8', errors='surrogateescape') as f:
        return f.read()

def write_text(p, s):
    with io.open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)


def md_blocks(text, want='('):
    """.md のトップレベルの括弧を (開始, 終了) の一覧で返す。
    want='(' で定義、want='[' でベクタ。

    **素朴な括弧数えでは壊れる。** 文字列やコメントの中の括弧まで数えて
    しまい、aarch64-simd.md を実際に壊した。
    ; コメント / " 文字列 / { } 波括弧文字列 を読み飛ばすこと。

    検証: 4.9.4 の 4 ファイルで「全ブロックが (define か (include で
    始まり、ブロックの外は空白とコメントだけ」を確認済み"""
    close = {'(': ')', '[': ']'}[want]
    out, i, n, depth, start = [], 0, len(text), 0, None
    while i < n:
        c = text[i]
        if c == ';':                                   # コメント
            j = text.find('\n', i)
            i = n if j < 0 else j + 1
            continue
        if c == '"':                                   # 文字列
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == '{':                                   # 波括弧文字列 (入れ子)
            b, i = 1, i + 1
            while i < n and b:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '{':
                    b += 1
                elif text[i] == '}':
                    b -= 1
                i += 1
            continue
        if c == want:
            if depth == 0:
                start = i
            depth += 1
        elif c == close:
            depth -= 1
            if depth == 0 and start is not None:
                out.append((start, i + 1))
                start = None
        i += 1
    return out

# int iterator を使う define_insn と一緒に消える "_internal" のうち、
# **呼ぶ側の expander が生き残ってしまう** もの。4.9.4 の定義は
#   (define_insn "aarch64_sq<r>dmulh_lane<mode>_internal")
# と int_attr <r> を含む名前で、呼ぶ側は
#   gen_aarch64_sqdmulh_lane<mode>_internal
# と展開後の名前で書く。**テンプレート同士の文字列照合では対応が付かない**
# (mode iterator 由来の aarch64_saddl<mode>_hi_internal 等は生きているので、
#  「定義が無い名前」で機械的に拾うと巻き込む)。よってリンカの実測で決めた。
#
# 取り直し方:
#   make all-gcc 2>&1 | grep -oE "undefined reference to .gen_[A-Za-z0-9_]+"
ORPHAN_EXPANDERS = ('aarch64_sqdmulh_lane<mode>', 'aarch64_sqrdmulh_lane<mode>',
                    'aarch64_sqdmulh_laneq<mode>', 'aarch64_sqrdmulh_laneq<mode>')

# ---- int iterator を使う定義だけを落とす ----
# **aarch64-simd.md を丸ごと消したら aarch64.c が 82 箇所で壊れた。**
# SIMD のパターンは mode iterator から生成されるので、aarch64.c が呼ぶ
# gen_aarch64_zip1v8qi のような名前は .md に文字列として現れない。
# 「NEON 専属の gen_* は 2 個」という最初の見積もりはこれで外した。
#
# 正しく測ると int iterator を使う定義は
#   aarch64-simd.md  328 個中  33 個 (frint / reduc_ / maxmin_uns / crypto)
#   aarch64.md       287 個中   2 個 (FP 丸め)
#   atomics.md        21 個中   0 個
# で、**zip / uzp / mov / dup といった aarch64.c が依存するものは含まれない。**
src_it = read_text(os.path.join(A, 'iterators.md'))
iters = sorted({m.group(1) for m in
                re.finditer(r'define_int_iterator\s+([A-Za-z0-9_]+)', src_it)})
attrs = sorted({m.group(1) for m in
                re.finditer(r'define_int_attr\s+([A-Za-z0-9_]+)', src_it)})

if iters:
    # **参照の形で照合する。** attr 名に 'r' や 'u' があるので、単純な語境界だと
    # 制約文字列 "r, w" に当たって aarch64_simd_dup まで巻き込む (実際に踏んだ)。
    # iterator は (unspec ... NAME) の裸の名前、attr は <name> の形でしか出ない
    pat_i = re.compile(r'(?<![A-Za-z0-9_<])(%s)(?![A-Za-z0-9_>])' % '|'.join(iters))
    pat_a = re.compile(r'<(%s)>' % '|'.join(attrs))

    for fn in ('aarch64.md', 'aarch64-simd.md'):
        p = os.path.join(A, fn)
        if not os.path.exists(p):
            continue
        s = read_text(p)
        drop = [(a, b) for a, b in md_blocks(s)
                if pat_i.search(s[a:b]) or pat_a.search(s[a:b])]
        for a, b in reversed(drop):          # 後ろから消すと位置がずれない
            s = s[:a] + s[b:]
        write_text(p, s)
        print("    %s: int iterator を使う定義を %d 個削除" % (fn, len(drop)))

    # ---- zip / uzp / trn だけは手で展開して戻す ----
    # **消える 33 個のうち aarch64.c が呼ぶのはこの 1 パターンだけ** (実測)。
    #   (define_insn "aarch64_<PERMUTE:perm_insn><PERMUTE:perm_hilo><mode>")
    # が zip1/zip2/trn1/trn2/uzp1/uzp2 の 6 種を生み、aarch64.c の
    # aarch64_expand_vec_perm が gen_aarch64_zip1v8qi などで直接呼ぶ。
    # mode iterator だけで書き直せば 4.7.4 でも通る。
    #
    # **定数の宣言は足さない。** UNSPEC_ZIP1 等は元から iterators.md の
    # define_c_enum "unspec" (行 199〜) にある。以前ここで aarch64.md 側にも
    # 足していたが、それは insn-constants.h を壊す:
    #
    #   aarch64.md の unspec 列挙が先に読まれ、足した 6 個が値 0〜5 を取る
    #     -> 続く iterators.md の同名を read_name が **定数展開** して
    #        名前が "0" 等の数字列に化ける (gcc/read-md.c:406)
    #     -> add_constant は別名の新規定数として登録するので
    #        「redefinition」検査 (read-md.c:708) が火を噴かない
    #     -> insn-constants.h に `0 = 112,` のような列挙子が出る
    #
    # 判定を aarch64.md だけで行っていたのが原因 (iterators.md を見ていない)。
    # **既に足してしまったツリーからも外せるよう、除去の向きで書く。**
    PERM = [('zip1', 'UNSPEC_ZIP1'), ('zip2', 'UNSPEC_ZIP2'),
            ('trn1', 'UNSPEC_TRN1'), ('trn2', 'UNSPEC_TRN2'),
            ('uzp1', 'UNSPEC_UZP1'), ('uzp2', 'UNSPEC_UZP2')]

    p = os.path.join(A, 'aarch64.md')
    s = read_text(p)
    dropped = 0
    for _, u in PERM:
        dup = '    %s\n' % u
        if dup in s:
            s = s.replace(dup, '', 1)
            dropped += 1
    if dropped:
        write_text(p, s)
        print("    aarch64.md: unspec 列挙の重複宣言 %d 個を除去" % dropped)
    else:
        print("    aarch64.md: unspec 列挙に重複なし。skip")

    p = os.path.join(A, 'aarch64-simd.md')
    s = read_text(p)
    if 'aarch64_zip1<mode>' not in s:
        body = ['\n;; zip / uzp / trn。**PERMUTE int iterator の手展開** '
                '(ports/port_aarch64_to_gcc47.sh が生成)。\n'
                ';; aarch64.c の aarch64_expand_vec_perm が gen_ で直接呼ぶので消せない。\n']
        for insn, unspec in PERM:
            body.append('''
(define_insn "aarch64_%s<mode>"
  [(set (match_operand:VALL 0 "register_operand" "=w")
\t(unspec:VALL [(match_operand:VALL 1 "register_operand" "w")
\t\t      (match_operand:VALL 2 "register_operand" "w")]
\t\t       %s))]
  "TARGET_SIMD"
  "%s\\\\t%%0.<Vtype>, %%1.<Vtype>, %%2.<Vtype>"
  [(set_attr "type" "neon_permute<q>")]
)
''' % (insn, unspec, insn))
        write_text(p, s + ''.join(body))
        print("    aarch64-simd.md: zip/uzp/trn を %d 個に手展開" % len(PERM))
    else:
        print("    aarch64-simd.md: 手展開は適用済み。skip")

    # ---- 孤児になった expander を落とす (ORPHAN_EXPANDERS 参照) ----
    # 呼び先の _internal が int iterator と一緒に消えたので、残すと
    #   insn-emit.c: undefined reference to `gen_aarch64_sqdmulh_lanev4hi_internal'
    # で cc1 のリンクが落ちる。**コンパイルは通るのでリンクまで出ない**
    p = os.path.join(A, 'aarch64-simd.md')
    s = read_text(p)
    pat_o = re.compile(r'\(define_expand\s+"(%s)"'
                       % '|'.join(re.escape(o) for o in ORPHAN_EXPANDERS))
    drop = [(a, b) for a, b in md_blocks(s) if pat_o.match(s[a:b])]
    for a, b in reversed(drop):
        s = s[:a] + s[b:]
    if drop:
        write_text(p, s)
        print("    aarch64-simd.md: 孤児になった expander を %d 個削除" % len(drop))
    else:
        print("    aarch64-simd.md: 孤児の expander は無い。skip")

# ---- define_expand の属性ベクタを落とす ----
# **4.8 で define_expand に属性ベクタが増えた。** rtl.def の書式文字列:
#   4.7.4  DEF_RTL_EXPR(DEFINE_EXPAND, "define_expand", "sEss",  RTX_EXTRA)
#   4.9.4  DEF_RTL_EXPR(DEFINE_EXPAND, "define_expand", "sEssV", RTX_EXTRA)
# 4.7.4 の読み手は名前 / パターン / 条件 / 準備文の 4 つを読んだら `)' を
# 期待するので、後ろに [(set_attr ...)] があると
#   aarch64-simd.md:2438: expected character `)', found `['
# で genpreds が止まる。**define_insn (sEsTV) と define_insn_and_split
# (sEsTsESV) は 4.7.4 でも V を持つので無事。** define_expand だけ。
#
# 実測: aarch64-simd.md の 96 個中 1 個 (aarch64_simd_combine<mode>) のみ。
# 属性は展開後の insn には残らないので落として構わない
print("--- define_expand の属性ベクタを落とす (4.7.4 は sEss)")
for fn in sorted(f for f in os.listdir(A) if f.endswith('.md')):
    p = os.path.join(A, fn)
    s = read_text(p)
    edits = []
    for a, b in md_blocks(s):
        if not s[a:b].startswith('(define_expand'):
            continue
        inner = s[a + 1:b - 1]
        # 1 個目の [ ] はパターンベクタ。**2 個目以降が 4.8 で増えた属性**
        for x, y in md_blocks(inner, '[')[1:]:
            edits.append((a + 1 + x, a + 1 + y))
    for x, y in reversed(edits):               # 後ろから消すと位置がずれない
        while x > 0 and s[x - 1] in ' \t':     # 直前の空白も
            x -= 1
        if y < len(s) and s[y] == '\n':        # 行ごと落とす
            y += 1
        s = s[:x] + s[y:]
    if edits:
        write_text(p, s)
        print("    %s: define_expand の属性ベクタを %d 個削除" % (fn, len(edits)))

# ---- iterators.md: 末尾の Int Iterators 節を切り落とす ----
# 実測で 26 個の define_int_iterator と 18 個の define_int_attr が
# **完全に連続**しており、間に必要な定義は 1 つも無い
p = os.path.join(A, 'iterators.md')
ls = lines(p)
i = next((k for k, l in enumerate(ls) if l.rstrip('\n') == ';; Int Iterators.'), None)
if i is None:
    print("    iterators.md: 適用済み。skip")
else:
    while i > 0 and ls[i - 1].startswith(';;'):   # 直前の罫線コメントも落とす
        i -= 1
    n = len(ls) - i
    write(p, ls[:i])
    print("    iterators.md: Int Iterators 節を削除 (%d 行)" % n)

# ---- aarch64-builtins.c をスタブに差し替える ----
# **aarch64.c は 1 行も触らない。** フック登録も aarch64-protos.h の宣言も
# そのまま使えるので、改変面積が最小になる。中身は SIMD 組み込み関数の
# 登録と展開だけで、CODE_FOR_aarch64_* (aarch64-simd.md 由来) に依存している
STUB = '''/* NEON を落とした構成のための空実装。

   本来の aarch64-builtins.c は SIMD 組み込み関数を登録・展開するが、中身が
   CODE_FOR_aarch64_* (aarch64-simd.md 由来) に依存しており、NEON を外すと
   コンパイルできない。**aarch64.c 側のフック登録を消さずに済ませるため**、
   同じ名前で無害な実装を置く。

   ports/port_aarch64_to_gcc47.sh が生成する。手で編集しないこと。  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "tree.h"
#include "expr.h"
#include "tm_p.h"
#include "recog.h"
#include "langhooks.h"
#include "diagnostic-core.h"
#include "optabs.h"

void
aarch64_init_builtins (void)
{
}

rtx
aarch64_expand_builtin (tree exp ATTRIBUTE_UNUSED,
\t\t\trtx target ATTRIBUTE_UNUSED,
\t\t\trtx subtarget ATTRIBUTE_UNUSED,
\t\t\tenum machine_mode mode ATTRIBUTE_UNUSED,
\t\t\tint ignore ATTRIBUTE_UNUSED)
{
  return NULL_RTX;
}

tree
aarch64_builtin_decl (unsigned code ATTRIBUTE_UNUSED,
\t\t      bool initialize_p ATTRIBUTE_UNUSED)
{
  return error_mark_node;
}

tree
aarch64_fold_builtin (tree fndecl ATTRIBUTE_UNUSED,
\t\t      int n_args ATTRIBUTE_UNUSED,
\t\t      tree *args ATTRIBUTE_UNUSED,
\t\t      bool ignore ATTRIBUTE_UNUSED)
{
  return NULL_TREE;
}

tree
aarch64_builtin_vectorized_function (tree fndecl ATTRIBUTE_UNUSED,
\t\t\t\t     tree type_out ATTRIBUTE_UNUSED,
\t\t\t\t     tree type_in ATTRIBUTE_UNUSED)
{
  return NULL_TREE;
}
'''
p = os.path.join(A, 'aarch64-builtins.c')
if 'NEON を落とした構成' not in open(p, encoding='utf-8', errors='replace').read():
    write(p, [STUB])
    print("    aarch64-builtins.c をスタブに差し替え")
else:
    print("    aarch64-builtins.c: 適用済み。skip")

# **配線証明: int iterator の参照が残っていないこと** ----------------------

left = []
for f in sorted(os.listdir(A)):
    if not f.endswith('.md'):
        continue
    src = open(os.path.join(A, f), encoding='utf-8', errors='replace').read()
    if re.search(r'define_int_(iterator|attr)', src):
        left.append(f)
if left:
    raise SystemExit("★ int iterator 定義が残っている: %s" % ', '.join(left))
print("    int iterator 定義の残りが無いことを確認")

# **配線証明: define_c_enum のメンバ名が重複していないこと** ----------------
# 重複は **エラーにならずに insn-constants.h を壊す**。read_name が 2 度目の
# 名前を定数展開して数字列に変えてしまうため、add_constant の redefinition
# 検査を素通りし、`0 = 112,` のような列挙子が生成される (上の PERM 参照)
seen, dups = {}, []
for f in sorted(os.listdir(A)):
    if not f.endswith('.md'):
        continue
    t = read_text(os.path.join(A, f))
    for m in re.finditer(r'\(define_c_enum\s+"([^"]+)"\s*\[', t):
        i = m.end() - 1
        j = t.index(']', i)
        body = re.sub(r';[^\n]*', '', t[i + 1:j])      # 行コメントを落とす
        for nm in body.split():
            key = (m.group(1), nm)
            if key in seen:
                dups.append('%s の %s (%s と %s)' % (m.group(1), nm, seen[key], f))
            else:
                seen[key] = f
if dups:
    raise SystemExit("★ define_c_enum のメンバが重複: %s" % '; '.join(dups))
print("    define_c_enum のメンバに重複が無いことを確認 (%d 個)" % len(seen))

# **配線証明: define_expand に属性ベクタが残っていないこと** ----------------
# 残っていると genpreds が「expected character `)', found `['」で止まる
left, total = [], 0
for f in sorted(os.listdir(A)):
    if not f.endswith('.md'):
        continue
    t = read_text(os.path.join(A, f))
    for a, b in md_blocks(t):
        if not t[a:b].startswith('(define_expand'):
            continue
        total += 1
        if len(md_blocks(t[a + 1:b - 1], '[')) > 1:
            left.append('%s:%d' % (f, t.count('\n', 0, a) + 1))
if left:
    raise SystemExit("★ define_expand に属性ベクタが残っている: %s" % ', '.join(left))
print("    define_expand に属性ベクタが無いことを確認 (%d 個)" % total)

# **配線証明: 孤児になった expander が残っていないこと** --------------------
left = []
for f in sorted(os.listdir(A)):
    if not f.endswith('.md'):
        continue
    t = read_text(os.path.join(A, f))
    for o in ORPHAN_EXPANDERS:
        if '"%s"' % o in t:
            left.append('%s: %s' % (f, o))
if left:
    raise SystemExit("★ 孤児 expander が残っている: %s" % ', '.join(left))
print("    孤児 expander %d 系統が残っていないことを確認" % len(ORPHAN_EXPANDERS))
PY

# --- 4.8 で変わった API の呼び出しを直す ----------------------------------
echo "--- 4.8 で変わった API の呼び出しを直す"
python3 - "$DST/gcc" <<'PY'
import io, os, re, sys
g = sys.argv[1]
A = os.path.join(g, 'config/aarch64')

def read(p):
    with io.open(p, encoding='utf-8', errors='surrogateescape') as f:
        return f.read()

def write(p, s):
    with io.open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(s)

# ---- plus_constant: 4.8 で mode 引数が先頭に増えた ----
#   4.7.4  rtx plus_constant (rtx, HOST_WIDE_INT)
#   4.8+   rtx plus_constant (enum machine_mode, rtx, HOST_WIDE_INT)
# **複数行にまたがる呼び出しがある**ので、単純な正規表現では切れない。
# 括弧の深さを数えて「最初の深さ 1 のコンマ」までを第 1 引数として落とす
def drop_first_arg(src, fname):
    out, i, n = [], 0, 0
    while True:
        k = src.find(fname, i)
        if k < 0:
            out.append(src[i:])
            break
        j = k + len(fname)
        while j < len(src) and src[j] in ' \t\n':
            j += 1
        if j >= len(src) or src[j] != '(':      # 呼び出しでない (宣言など)
            out.append(src[i:j])
            i = j
            continue
        depth, m = 1, j + 1
        while m < len(src) and depth:
            c = src[m]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif c == ',' and depth == 1:
                break
            m += 1
        if m >= len(src) or src[m] != ',':      # 引数が 1 つ = 既に直っている
            out.append(src[i:j + 1])
            i = j + 1
            continue
        m += 1
        while m < len(src) and src[m] in ' \t\n':
            m += 1
        out.append(src[i:j + 1])
        i = m
        n += 1
    return ''.join(out), n

total = 0
for f in ('aarch64.c', 'aarch64.md'):
    p = os.path.join(A, f)
    s = read(p)
    s2, n = drop_first_arg(s, 'plus_constant')
    if n:
        write(p, s2)
        total += n
    print("    %s: plus_constant の mode 引数を %d 箇所削除" % (f, n))

# ---- GET_MODE_UNIT_BITSIZE: 4.8 で machmode.h に入ったマクロ ----
# **4.7.4 には無い。** aarch64.h は tm.h なので、CLZ_DEFINED_VALUE_AT_ZERO を
# 展開する core 側 (builtins.c / dwarf2out.c / optabs.c / rtlanal.c /
# simplify-rtx.c) で暗黙の関数呼び出しになり、cc1 のリンクで
#   libbackend.a(builtins.o): undefined reference to `GET_MODE_UNIT_BITSIZE'
# になる。**-w が効いているのでコンパイルは黙って通る。リンクまで出ない。**
# バックエンドのマクロが tm.h 経由で core を汚す形なので、直すのは aarch64.h 側。
# 4.9.4 の定義をその場に展開する (4.7.4 の GET_MODE_BITSIZE と同じ書き方)
p = os.path.join(A, 'aarch64.h')
s = read(p)
OLD_UB = 'GET_MODE_UNIT_BITSIZE (MODE)'
NEW_UB = '((unsigned short) (GET_MODE_UNIT_SIZE (MODE) * BITS_PER_UNIT))'
n = s.count(OLD_UB)
if n:
    write(p, s.replace(OLD_UB, NEW_UB))
    print("    aarch64.h: GET_MODE_UNIT_BITSIZE を 4.7.4 の式に展開 (%d 箇所)" % n)
else:
    print("    aarch64.h: GET_MODE_UNIT_BITSIZE は既に無い")

# ---- 4.9 で入った tree / rtl のアクセサ ----
#   4.9 の呼び方               4.7.4 の呼び方
#   tree_fits_uhwi_p (T)       host_integerp (T, 1)
#   tree_to_uhwi (T)           tree_low_cst (T, 1)
#   tree_to_shwi (T)           tree_low_cst (T, 0)
#   add_int_reg_note (I,K,V)   add_reg_note (I, K, GEN_INT (V))
# **どれも引数の数か形が変わるので綴りの置換では済まない。**
# 宣言が無いだけなのでコンパイルは通り (-w)、リンクで初めて出る


def rewrite_call(src, fname, newname, transform):
    """fname (...) を探し、深さ 1 のコンマで引数に割って transform に渡す。
    **複数行にまたがる呼び出しがある**ので正規表現では切れない"""
    pat = re.compile(r'\b%s\s*\(' % re.escape(fname))
    out, i, n = [], 0, 0
    while True:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:])
            break
        depth, k, args, start = 1, m.end(), [], m.end()
        while k < len(src) and depth:
            c = src[k]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    args.append(src[start:k])
                    break
            elif c == ',' and depth == 1:
                args.append(src[start:k])
                start = k + 1
            k += 1
        out.append(src[i:m.start()])
        out.append('%s (%s)'
                   % (newname, ', '.join(transform([a.strip() for a in args]))))
        i = k + 1
        n += 1
    return ''.join(out), n


SUBS = (('tree_fits_uhwi_p', 'host_integerp', lambda a: a + ['1']),
        ('tree_to_uhwi',     'tree_low_cst',  lambda a: a + ['1']),
        ('tree_to_shwi',     'tree_low_cst',  lambda a: a + ['0']),
        ('add_int_reg_note', 'add_reg_note',
         lambda a: a[:2] + ['GEN_INT (%s)' % a[2]]))
p = os.path.join(A, 'aarch64.c')
s = read(p)
done = []
for old, new, tr in SUBS:
    s, n = rewrite_call(s, old, new, tr)
    if n:
        done.append('%s -> %s %d' % (old, new, n))
if done:
    write(p, s)
    print("    aarch64.c: 4.9 のアクセサを 4.7.4 の形に (%s)" % ' / '.join(done))
else:
    print("    aarch64.c: 4.9 のアクセサは既に無い")

# ---- aarch64-protos.h: 落としたフックの宣言を消す ----
# TARGET_GIMPLE_FOLD_BUILTIN は 4.7.4 に無いので登録を外してある。
# 宣言だけ残ると gimple_stmt_iterator が未定義で gencondmd が通らない
# (gencondmd は gimple.h を取り込まないため)
p = os.path.join(A, 'aarch64-protos.h')
ls = read(p).split('\n')
before = len(ls)
ls = [l for l in ls if 'aarch64_gimple_fold_builtin' not in l]
if len(ls) != before:
    write(p, '\n'.join(ls))
    print("    aarch64-protos.h: aarch64_gimple_fold_builtin の宣言を削除")
else:
    print("    aarch64-protos.h: 適用済み。skip")

# ---- 4.9 で tree.h から分割されたヘッダの取り込みを落とす ----
# 4.9 は巨大だった tree.h を calls.h / stringpool.h / varasm.h ... へ分割した。
# **4.7.4 では全部 tree.h に入っている**ので、取り込み行を消せばよい
SPLIT = ('calls.h', 'gimple-expr.h', 'gimplify.h', 'hash-table.h', 'is-a.h',
         'stor-layout.h', 'stringpool.h', 'tree-eh.h', 'varasm.h')
for f in ('config/aarch64/aarch64.c', 'config/aarch64/aarch64-common.c',
          'config/arm/aarch-common.c'):
    p = os.path.join(g, f)
    if not os.path.exists(p):
        continue
    ls = read(p).split('\n')
    keep = [l for l in ls
            if not any(l.strip() == '#include "%s"' % h for h in SPLIT)]
    if len(keep) != len(ls):
        write(p, '\n'.join(keep))
        print("    %s: 4.9 で分割されたヘッダの取り込みを %d 行削除"
              % (os.path.basename(f), len(ls) - len(keep)))

# ---- crtl->is_leaf: 4.8 で新設。4.7.4 は output.h の大域変数 ----
p = os.path.join(A, 'aarch64.c')
s = read(p)
n = s.count('crtl->is_leaf')
if n:
    write(p, s.replace('crtl->is_leaf', 'current_function_is_leaf'))
    print("    aarch64.c: crtl->is_leaf を current_function_is_leaf に (%d 箇所)" % n)

# ---- assign_stack_temp: 4.8 で第 3 引数 (keep) が消えた ----
#   4.7.4  rtx assign_stack_temp (enum machine_mode, HOST_WIDE_INT, int)
#   4.8+   rtx assign_stack_temp (enum machine_mode, HOST_WIDE_INT)
s = read(p)
s2 = re.sub(r'assign_stack_temp \(mode, GET_MODE_SIZE \(mode\)\)',
            'assign_stack_temp (mode, GET_MODE_SIZE (mode), 0)', s)
if s2 != s:
    write(p, s2)
    print("    aarch64.c: assign_stack_temp に第 3 引数を補った")

# ---- aarch64_add_stmt_cost: 4.8 のベクトル化コストモデル。丸ごと不要 ----
# TARGET_VECTORIZE_ADD_STMT_COST の登録は既に外してある。関数だけ残ると
# enum vect_cost_model_location / vect_body / vec_construct が未定義で落ちる
ls = read(p).split('\n')
i = next((k for k, l in enumerate(ls)
          if l.startswith('aarch64_add_stmt_cost ')), None)
if i is not None:
    while i > 0 and (ls[i - 1].startswith('static') or ls[i - 1].startswith('/*')
                     or ls[i - 1].startswith(' ')):
        i -= 1                                  # 直前の static 行とコメントも
    e = next(k for k in range(i, len(ls)) if ls[k] == '}')
    del ls[i:e + 1]
    write(p, '\n'.join(ls))
    print("    aarch64.c: aarch64_add_stmt_cost を削除 (%d 行)" % (e - i + 1))
else:
    print("    aarch64.c: aarch64_add_stmt_cost は既に無い")

# ---- vec_construct: 4.8 で enum vect_cost_for_stmt に増えた列挙子 ----
# 4.7.4 の enum は target.h:134 にあり 13 個。**vec_construct だけが無い**:
#   4.7.4  scalar_stmt … vec_perm, vec_promote_demote        13 個
#   4.9.4  + vec_construct                                   14 個
# aarch64_builtin_vectorization_cost が case で受けているので
#   aarch64.c:4938: error: 'vec_construct' undeclared
# で止まる。**4.7.4 のベクトル化器はこの原価を問い合わせない**ので arm ごと
# 落として良い。core の target.h に足す手もあるが、バックエンドの外に
# 触らない方針を保つ。唯一の使い手が消えるので宣言 (unsigned elements;) も落とす
p = os.path.join(A, 'aarch64.c')
ls = read(p).split('\n')
i = next((k for k, l in enumerate(ls) if l.strip() == 'case vec_construct:'), None)
if i is None:
    print("    aarch64.c: vec_construct の case は既に無い")
else:
    e = next(k for k in range(i + 1, len(ls)) if ls[k].strip().startswith('return '))
    if e + 1 < len(ls) and ls[e + 1].strip() == '':
        e += 1                                    # 続く空行も
    del ls[i:e + 1]
    # **宣言を消すのは、その関数の中に他の使い手が無いときだけ**
    b = next(k for k, l in enumerate(ls)
             if l.startswith('aarch64_builtin_vectorization_cost '))
    z = next(k for k in range(b, len(ls)) if ls[k] == '}')
    rest = [l for l in ls[b:z + 1] if l.strip() != 'unsigned elements;']
    if any(re.search(r'\belements\b', l) for l in rest):
        print("    aarch64.c: vec_construct の case を削除 (宣言は使用中なので残す)")
    else:
        d = next(k for k in range(b, z) if ls[k].strip() == 'unsigned elements;')
        n = 2 if ls[d + 1].strip() == '' else 1
        del ls[d:d + n]
        print("    aarch64.c: vec_construct の case と unsigned elements; を削除")
    write(p, '\n'.join(ls))

# ---- simd_immediate_info の前方宣言 ----
# **C と C++ の差。** 4.9 は C++ でビルドされるので、引数リストの中で
# struct を初出させても名前空間スコープに入る。**C では引数リスト限りの
# スコープ**になり、ファイル先頭の定義と別の型になって conflicting types
p = os.path.join(A, 'aarch64-protos.h')
s = read(p)
GUARD = '#define GCC_AARCH64_PROTOS_H\n'
if 'struct simd_immediate_info;' not in s:
    s = s.replace(GUARD, GUARD + '\n/* 引数リストの中で初出させないための前方宣言 (C ビルドのため)。  */\n'
                         'struct simd_immediate_info;\n', 1)
    write(p, s)
    print("    aarch64-protos.h: struct simd_immediate_info を前方宣言")
else:
    print("    aarch64-protos.h: 前方宣言は適用済み。skip")

# ---- rtx_code: 4.9 は typedef、4.7.4 は enum を書く必要がある ----
p = os.path.join(g, 'config/arm/aarch-common.c')
s = read(p)
s2 = re.sub(r'(?<![_A-Za-z])(?<!enum )rtx_code(?![_A-Za-z])', 'enum rtx_code', s)
if s2 != s:
    write(p, s2)
    print("    aarch-common.c: 裸の rtx_code を enum rtx_code に (%d 箇所)"
          % len(re.findall(r'(?<![_A-Za-z])(?<!enum )rtx_code(?![_A-Za-z])', s)))
else:
    print("    aarch-common.c: 適用済み。skip")

# **配線証明: 3 引数の plus_constant が残っていないこと** ------------------

left = []
for f in ('aarch64.c', 'aarch64.md'):
    s = read(os.path.join(A, f))
    # plus_constant ( のあと最初の深さ 1 のコンマまでに mode らしき語が来る形
    for m in re.finditer(r'plus_constant\s*\(\s*(Pmode|mode|[A-Z]+mode)\s*,', s):
        left.append('%s: %s' % (f, m.group(0).replace('\n', ' ')))
if left:
    raise SystemExit("★ plus_constant が 3 引数のまま: %s" % '; '.join(left[:5]))
print("    plus_constant の残り (3 引数形) が無いことを確認")

# **配線証明: 4.7.4 に無い列挙子・識別子が残っていないこと** ----------------
# 綴りだけで消えたつもりになるのを防ぐ。増えたらここに足す
# **リンクまで出ない類がここに効く。** 綴りが残っていてもコンパイルは
# -w で黙って通り、cc1 のリンクで undefined reference になる
GONE_47 = ('vec_construct',           # 4.8 で enum vect_cost_for_stmt に追加
           'crtl->is_leaf',           # 4.8 で新設。4.7.4 は current_function_is_leaf
           'aarch64_add_stmt_cost',
           'GET_MODE_UNIT_BITSIZE',   # 4.8 で machmode.h に追加
           'tree_fits_uhwi_p',        # 4.9。4.7.4 は host_integerp (T, 1)
           'tree_to_uhwi',            # 4.9。4.7.4 は tree_low_cst (T, 1)
           'tree_to_shwi',            # 4.9。4.7.4 は tree_low_cst (T, 0)
           'add_int_reg_note')        # 4.9。4.7.4 は add_reg_note + GEN_INT
left = []
for f in sorted(os.listdir(A)):
    if not (f.endswith('.c') or f.endswith('.h')):
        continue
    s = read(os.path.join(A, f))
    for g_ in GONE_47:
        if g_ in s:
            left.append('%s: %s' % (f, g_))
if left:
    raise SystemExit("★ 4.7.4 に無い識別子が残っている: %s" % ', '.join(left))
print("    4.7.4 に無い識別子 (%s) が残っていないことを確認" % ', '.join(GONE_47))
PY

echo
echo "=== 移植完了。$DST ==="
