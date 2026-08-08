#!/bin/bash
# GCC 4.6.4 フォークの RISC-V バックエンドを GCC 4.7.4 へ前方移植する。
#
# なぜ 4.7.4 か:
#   - 4.7.4 は「C だけでビルドできる最後の GCC」かつ「__atomic_* を持つ最初の GCC」
#   - フォークの gcc/config/riscv/sync.md は atomic_store<mode> / atomic_exchange<mode>
#     など **4.7 以降の optab 名**で書かれている (上流 GCC 7 由来)。
#     4.6.4 には sync_* optab しか無いので atomic が libcall にしかならないが、
#     4.7.4 には atomic_* optab が揃っているのでそのまま inline になる
#   - x86 側が既に 4.7.4 を使っているので版が揃う
#
# 既存の ports/gcc-4.7.4 (x86 ネイティブ用・ビルド済み) は触らない。
set -e

PORTS="$(cd "$(dirname "$0")" && pwd)"
FORK="$PORTS/gcc-4.6.4-riscv"
DST="$PORTS/gcc-4.7.4-riscv"
TARBALL="$PORTS/gcc-4.7.4.tar.gz"

[ -d "$FORK" ] || { echo "error: $FORK が無い"; exit 1; }

if [ ! -d "$DST" ]; then
  echo "--- gcc-4.7.4 を $DST に展開"
  mkdir -p "$DST"
  tar -xzf "$TARBALL" -C "$DST" --strip-components=1
fi

# --- バックエンド本体をそのまま持ち込む ---------------------------------
echo "--- gcc/config/riscv と libgcc/config/riscv を複製"
rm -rf "$DST/gcc/config/riscv" "$DST/libgcc/config/riscv"
cp -a "$FORK/gcc/config/riscv"    "$DST/gcc/config/riscv"
cp -a "$FORK/libgcc/config/riscv" "$DST/libgcc/config/riscv"

# --- ビルド設定へのハンクを機械的に移す ---------------------------------
# 手で写すと取りこぼすので、フォーク側からブロックを切り出して挿入する。
python3 - "$FORK" "$DST" <<'PY'
import sys, io, os
fork, dst = sys.argv[1], sys.argv[2]

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

def cut(ls, begin, end_exclusive):
    """begin 行から end_exclusive 行の直前までを切り出す"""
    b = find(ls, begin)
    e = find(ls, end_exclusive, b)
    return ls[b:e]

def insert_before(path, anchor, block, tag):
    ls = lines(path)
    if any(l.startswith('riscv') or l.startswith('\triscv') for l in ls) and tag in ''.join(ls):
        print("    %s: 適用済み。skip" % os.path.basename(path))
        return
    i = find(ls, anchor)
    write(path, ls[:i] + block + ls[i:])
    print("    %s: %s を %d 行目の前に挿入" % (os.path.basename(path), tag, i + 1))

# ---- gcc/config.gcc : 3 箇所 ----
f_cfg = lines(os.path.join(fork, 'gcc/config.gcc'))
blk_cpu    = cut(f_cfg, 'riscv*)',        'rs6000*-*-*)')
blk_target = cut(f_cfg, 'riscv*-*-linux*)', 'pdp11-*-*)')
blk_defs   = cut(f_cfg, '\triscv*-*-*)',   '\tmips*-*-*)')

d_cfg_p = os.path.join(dst, 'gcc/config.gcc')
d_cfg = lines(d_cfg_p)
if 'cpu_type=riscv' in ''.join(d_cfg):
    print("    config.gcc: 適用済み。skip")
else:
    # 後ろから挿すと前の行番号がずれない
    i3 = find(d_cfg, '\tmips*-*-*)')          # supported_defaults の側
    d_cfg = d_cfg[:i3] + blk_defs + d_cfg[i3:]
    i2 = find(d_cfg, 'pdp11-*-*)')
    d_cfg = d_cfg[:i2] + blk_target + d_cfg[i2:]
    i1 = find(d_cfg, 'rs6000*-*-*)')
    d_cfg = d_cfg[:i1] + blk_cpu + d_cfg[i1:]
    write(d_cfg_p, d_cfg)
    print("    config.gcc: cpu_type / target / with-arch 推論 の 3 ブロックを挿入")

# ---- libgcc/config.host : 2 箇所 ----
f_host = lines(os.path.join(fork, 'libgcc/config.host'))
blk_hcpu  = cut(f_host, 'riscv*-*-*)',   'sparc64*-*-*)')
blk_htm   = cut(f_host, 'riscv*-*-elf)', 'rs6000-ibm-aix4.[3456789]* | powerpc-ibm-aix4.[3456789]*)')

d_host_p = os.path.join(dst, 'libgcc/config.host')
d_host = lines(d_host_p)
if 'cpu_type=riscv' in ''.join(d_host):
    print("    config.host: 適用済み。skip")
else:
    i2 = find(d_host, 'rx-*-elf)')
    d_host = d_host[:i2] + blk_htm + d_host[i2:]
    i1 = find(d_host, 'rs6000*-*-*)')
    d_host = d_host[:i1] + blk_hcpu + d_host[i1:]
    write(d_host_p, d_host)
    print("    config.host: cpu_type / tmake の 2 ブロックを挿入")

# ---- config.sub : CPU 一覧 と musl 対応 ----
# 4.7.4 の config.sub は musl を知らない (musl 対応は後の版で入った)。
# 知らないと riscv64-linux-musl が basic_machine='riscv64-linux' と誤分割され、
# 「machine `riscv64-linux' not recognized」で configure が落ちる。
# 128 行目の maybe_os に linux-musl* を足すと basic_machine='riscv64' に正しく割れる。
p = os.path.join(dst, 'config.sub')
ls = lines(p)
changed = []

if not any('riscv' in l for l in ls):
    # 4.6.4 では '| rx \' 単独行だが 4.7.4 では '| rl78 | rx \' にまとまっている
    i = find(ls, '\t| rl78 | rx \\')
    ls = ls[:i+1] + ['\t| riscv32 | riscv64 \\\n'] + ls[i+1:]
    changed.append('riscv32/riscv64 を CPU 一覧に追加')

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
PY

# --- 4.6 → 4.7 の target hook 差分 ---------------------------------------
# 4.7 でオプション処理が targetm から targetm_common に分離され、
# TARGET_HANDLE_OPTION / TARGET_OPTION_OPTIMIZATION_TABLE は
# gcc/common/config/<cpu>/<cpu>-common.c に置くことが必須になった
# (config.gcc の target_has_targetm_common が既定 yes なので、無いとビルドが
#  「No rule to make target .../riscv-common.c」で止まる)。
echo "--- gcc/common/config/riscv/riscv-common.c を設置"
mkdir -p "$DST/gcc/common/config/riscv"
cp "$PORTS/riscv-common.c.for47" "$DST/gcc/common/config/riscv/riscv-common.c"

echo "--- riscv.c から移動済みのフックを取り除く"
python3 - "$DST" <<'PY'
import sys, io, os
dst = sys.argv[1]
p = os.path.join(dst, 'gcc/config/riscv/riscv.c')
with io.open(p, encoding='utf-8', errors='surrogateescape') as f:
    ls = f.readlines()

if 'riscv_handle_option' not in ''.join(ls):
    print("    riscv.c: 適用済み。skip")
    raise SystemExit(0)

def drop_block(ls, start_marker, end_pred, label):
    """start_marker で始まり end_pred が真になる行までを削除する"""
    for i, l in enumerate(ls):
        if l.rstrip('\n') == start_marker:
            for j in range(i + 1, len(ls)):
                if end_pred(ls[j]):
                    del ls[i:j + 1]
                    print("    riscv.c: %s を削除 (%d 行)" % (label, j + 1 - i))
                    return ls
            break
    raise SystemExit("riscv.c: %s が見つからない" % label)

# 関数・テーブル本体 (末尾は桁 0 の '}' か '  };')
ls = drop_block(ls, '/* Implement TARGET_OPTION_OPTIMIZATION_TABLE.  */',
                lambda l: l.rstrip('\n') == '  };', 'option_optimization_table')
ls = drop_block(ls, '/* Parse a RISC-V ISA string into an option mask.  */',
                lambda l: l.rstrip('\n') == '}', 'riscv_parse_arch_string')
ls = drop_block(ls, '/* Implement TARGET_HANDLE_OPTION.  */',
                lambda l: l.rstrip('\n') == '}', 'riscv_handle_option')

# フック登録の #undef/#define の対
out, i, removed = [], 0, 0
while i < len(ls):
    s = ls[i].rstrip('\n')
    if s in ('#undef TARGET_OPTION_OPTIMIZATION_TABLE', '#undef TARGET_HANDLE_OPTION'):
        i += 2                      # #undef と #define の 2 行を飛ばす
        while i < len(ls) and ls[i].strip() == '':
            i += 1                  # 続く空行も落とす
        removed += 1
        continue
    out.append(ls[i]); i += 1
print("    riscv.c: フック登録 %d 組を削除" % removed)

# ---- 4.7 で変わった API の呼び出しを直す ----
# どちらも 4.6 → 4.7 で引数が増えたもの。
#   cannot_force_const_mem: (x) → (mode, x)
#   rtx_cost:               (x, outer_code, speed) → (x, outer_code, opno, speed)
# (TARGET_LEGITIMATE_CONSTANT_P フックはフォークの riscv.c に既にある。
#  4.7 で廃止されたのは riscv.h 側の同名マクロだけなので、そちらだけ落とす)
body = ''.join(out)

# フック本体の signature も 4.7 で変わっている。
#
# TARGET_RTX_COSTS: 4.7 は (x, code, outer_code, opno, *total, speed) の 6 引数。
# フォークは 5 引数なので引数がずれ、*total として別の値を書きに行って
# cc1 が init_expmed で SIGSEGV する (空ファイルでも落ちる)。
#
# ついでにフォークの第 2 引数の型が誤っている点も直す。4.6 の第 2 引数は
# rtx の code (int) なのに enum machine_mode mode と宣言し、本体で 25 箇所
# machine_mode として使っていた。4.7 でも第 2 引数は code なので、
# mode は GET_MODE (x) から取り直す。上流 GCC 7 の意味に合う形になる。
old_sig = 'riscv_rtx_costs (rtx x, enum machine_mode mode, int outer_code, int *total, bool speed)\n{\n'
new_sig = ('riscv_rtx_costs (rtx x, int code ATTRIBUTE_UNUSED, int outer_code,\n'
           '\t\t int opno ATTRIBUTE_UNUSED, int *total, bool speed)\n'
           '{\n'
           '  enum machine_mode mode = GET_MODE (x);\n')
if old_sig not in body:
    raise SystemExit("riscv.c: riscv_rtx_costs の signature が想定と違う")
body = body.replace(old_sig, new_sig)
print("    riscv.c: riscv_rtx_costs を 4.7 の 6 引数に (mode は GET_MODE から取得)")

# TARGET_CANNOT_FORCE_CONST_MEM: 4.7 は (mode, x) の 2 引数
old_c = 'riscv_cannot_force_const_mem (rtx x)\n'
new_c = 'riscv_cannot_force_const_mem (enum machine_mode mode ATTRIBUTE_UNUSED, rtx x)\n'
if old_c not in body:
    raise SystemExit("riscv.c: riscv_cannot_force_const_mem の signature が想定と違う")
body = body.replace(old_c, new_c)
print("    riscv.c: riscv_cannot_force_const_mem を 4.7 の 2 引数に")

n_cfcm = body.count('targetm.cannot_force_const_mem (src)')
body = body.replace('targetm.cannot_force_const_mem (src)',
                    'targetm.cannot_force_const_mem (mode, src)')
n_cost = body.count(', SET, speed)')
body = body.replace(', SET, speed)', ', SET, 0, speed)')
print("    riscv.c: cannot_force_const_mem に mode を追加 (%d 箇所)" % n_cfcm)
print("    riscv.c: rtx_cost に opno を追加 (%d 箇所)" % n_cost)

with io.open(p, 'w', encoding='utf-8', errors='surrogateescape') as f:
    f.write(body)

# ---- riscv.h: 廃止マクロを落とす ----
h = os.path.join(dst, 'gcc/config/riscv/riscv.h')
with io.open(h, encoding='utf-8', errors='surrogateescape') as f:
    hs = f.readlines()
n = len(hs)
hs = [l for l in hs if not l.startswith('#define LEGITIMATE_CONSTANT_P')]
if len(hs) != n:
    with io.open(h, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(hs)
    print("    riscv.h: LEGITIMATE_CONSTANT_P の定義を削除")

# ---- linux.h: MD_UNWIND_SUPPORT は 4.7 で libgcc 側の md_unwind_header へ ----
lh = os.path.join(dst, 'gcc/config/riscv/linux.h')
with io.open(lh, encoding='utf-8', errors='surrogateescape') as f:
    lhs = f.readlines()
n = len(lhs)
lhs = [l for l in lhs if not l.startswith('#define MD_UNWIND_SUPPORT')]
if len(lhs) != n:
    with io.open(lh, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(lhs)
    print("    linux.h: MD_UNWIND_SUPPORT の定義を削除")

# 4.7 には LINUX_DYNAMIC_LINKER (実行時 libc を選ぶ仕組み) が無い。4.7 の名前は
# GNU_USER_DYNAMIC_LINKER で、musl の選択肢自体が存在しない (musl 対応は GCC 6 で
# 上流入りした)。フォークの gcc/config/linux.h にはその backport が入っているが、
# 4.7 へ持ち込むには linux.opt / config.gcc の DEFAULT_LIBC まで一式が要る。
#
# Orthox は riscv64 も musl ユーザーランドも**静的リンクのみ** (MUSL_CONFIGURE_EXTRA
# に --disable-shared、リンクは -static) なので動的リンカ名は使われない。
# triple が riscv64-linux-musl である以上 musl を指すのが正しいので、そう固定する。
# 4.7 では OS 共通のマクロが LINUX_ から GNU_USER_ に改名されている。
# 未定義のまま使うと暗黙関数扱いでコンパイルが通り、リンクで初めて落ちる。
for i, l in enumerate(lhs):
    if 'LINUX_TARGET_OS_CPP_BUILTINS' in l:
        lhs[i] = l.replace('LINUX_TARGET_OS_CPP_BUILTINS',
                           'GNU_USER_TARGET_OS_CPP_BUILTINS')
        print("    linux.h: LINUX_TARGET_OS_CPP_BUILTINS → GNU_USER_ に改名")

if not any('LINUX_DYNAMIC_LINKER MUSL_DYNAMIC_LINKER' in l for l in lhs):
    for i, l in enumerate(lhs):
        if l.startswith('#define LINK_SPEC'):
            lhs[i:i] = [
                '/* 4.7 に LINUX_DYNAMIC_LINKER は無い。Orthox は静的リンクのみなので\n',
                '   実際には使われないが、triple が -musl なので musl を指す。  */\n',
                '#ifndef LINUX_DYNAMIC_LINKER\n',
                '#define LINUX_DYNAMIC_LINKER MUSL_DYNAMIC_LINKER\n',
                '#endif\n',
                '\n',
            ]
            print("    linux.h: LINUX_DYNAMIC_LINKER を musl に固定")
            break
    with io.open(lh, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(lhs)

# libgcc 側で unwind ヘッダを指定し直す
ch = os.path.join(dst, 'libgcc/config.host')
with io.open(ch, encoding='utf-8', errors='surrogateescape') as f:
    cs = f.readlines()
if not any('riscv/linux-unwind.h' in l for l in cs):
    for i, l in enumerate(cs):
        if l.rstrip('\n') == 'riscv*-*-linux*)':
            cs.insert(i + 2, '\tmd_unwind_header=riscv/linux-unwind.h\n')
            print("    config.host: md_unwind_header=riscv/linux-unwind.h を追加")
            break
    with io.open(ch, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(cs)

# ---- riscv.md: simple_return を SIMPLE_RETURN rtx に戻す ----
# 上流 GCC 7 の riscv.md は return 系を (simple_return) で書いている。
# フォークは 4.6 に SIMPLE_RETURN rtx が無いので (return) に書き換えてあるが、
# **4.7 には SIMPLE_RETURN がある**。名前 simple_return と中身 (return) が
# 食い違ったままだと、シブリングコール最適化 (-O2 の
# -foptimize-sibling-calls) で JUMP_LABEL に別々の rtx が入り
#   internal compiler error: in mark_jump_label_1, at jump.c:1092
# で落ちる (busybox の shell/ash.c freeparam などで実際に踏んだ)。
md = os.path.join(dst, 'gcc/config/riscv/riscv.md')
with io.open(md, encoding='utf-8', errors='surrogateescape') as f:
    ms = f.read()
md_reps = [
    ('(define_expand "return"\n  [(return)]\n',
     '(define_expand "return"\n  [(simple_return)]\n'),
    ('(define_insn "simple_return"\n  [(return)]\n',
     '(define_insn "simple_return"\n  [(simple_return)]\n'),
    ('(define_insn "simple_return_internal"\n  [(return)\n',
     '(define_insn "simple_return_internal"\n  [(simple_return)\n'),
]
n_md = 0
for old, new in md_reps:
    if old in ms:
        ms = ms.replace(old, new)
        n_md += 1
if n_md:
    with io.open(md, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(ms)
    print("    riscv.md: return → simple_return rtx (%d 箇所)" % n_md)

# ---- hwint.h: sext_hwi を補う ----
# sext_hwi は GCC 4.8 以降のヘルパで 4.7.4 には無い。フォークは 4.6.4 の hwint.h に
# 足しているので、同じブロックをそのまま持ってくる (バックエンドで 6 箇所使う)。
# ctz_hwi / clz_hwi / HOST_WIDE_INT_1 は 4.7.4 にも既にある。
hw_dst = os.path.join(dst, 'gcc/hwint.h')
with io.open(hw_dst, encoding='utf-8', errors='surrogateescape') as f:
    hw = f.readlines()
if not any('sext_hwi' in l for l in hw):
    hw_src = os.path.join(os.path.dirname(dst), 'gcc-4.6.4-riscv/gcc/hwint.h')
    with io.open(hw_src, encoding='utf-8', errors='surrogateescape') as f:
        fs = f.readlines()
    b = next(i for i, l in enumerate(fs)
             if l.rstrip('\n') == '/* Sign extend SRC starting from PREC.  */')
    e = next(i for i in range(b + 1, len(fs)) if fs[i].rstrip('\n') == '}')
    blk = fs[b:e + 1] + ['\n']
    g = next(i for i, l in enumerate(hw)
             if l.rstrip('\n') == '#endif /* ! GCC_HWINT_H */')
    hw[g:g] = blk
    with io.open(hw_dst, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(hw)
    print("    hwint.h: sext_hwi を追加 (%d 行)" % len(blk))

# ---- gcc/configure(.ac): TLS 検査と cpu_type 一覧 ----
# GCC の configure は「アセンブラが TLS を扱えるか」をターゲット別の case で調べる。
# riscv の case が無いと検査自体が走らず HAVE_AS_TLS が undef のままになり、
# __thread が emutls (__emutls_get_address) に落ちる。musl は native TLS 前提なので致命的。
# ビルドは成功してしまい生成コードだけが変わるので気付きにくい。
f_cfgr = os.path.join(os.path.dirname(dst), 'gcc-4.6.4-riscv/gcc/configure')
with io.open(f_cfgr, encoding='utf-8', errors='surrogateescape') as f:
    fc = f.readlines()
b = next(i for i, l in enumerate(fc) if l.rstrip('\n') == '  riscv*-*-*)')
e = next(i for i in range(b + 1, len(fc)) if fc[i].rstrip('\n') == '\t;;')
tls_blk = fc[b:e + 1]

for name in ('configure', 'configure.ac'):
    p2 = os.path.join(dst, 'gcc', name)
    with io.open(p2, encoding='utf-8', errors='surrogateescape') as f:
        cc = f.readlines()
    if any('riscv' in l for l in cc):
        print("    gcc/%s: 適用済み。skip" % name)
        continue
    i = next(k for k, l in enumerate(cc) if l.rstrip('\n') == '  s390-*-*)')
    cc[i:i] = tls_blk
    # cpu_type 一覧 (アセンブラ検査用の nop を出せる CPU) にも riscv を足す
    old = '  | pa | rs6000 | score | sparc | spu | tilegx | tilepro | xstormy16 | xtensa)\n'
    new = '  | pa | riscv | rs6000 | score | sparc | spu | tilegx | tilepro | xstormy16 | xtensa)\n'
    n2 = sum(1 for l in cc if l == old)
    cc = [new if l == old else l for l in cc]
    with io.open(p2, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.writelines(cc)
    print("    gcc/%s: TLS 検査の riscv case を追加 / cpu_type 一覧に riscv (%d 箇所)"
          % (name, n2))

# linux-unwind.h は gcc/config/riscv から libgcc/config/riscv へ移す
src_u = os.path.join(dst, 'gcc/config/riscv/linux-unwind.h')
dst_u = os.path.join(dst, 'libgcc/config/riscv/linux-unwind.h')
if os.path.exists(src_u) and not os.path.exists(dst_u):
    import shutil
    shutil.copy2(src_u, dst_u)
    print("    libgcc/config/riscv/linux-unwind.h を配置")
PY

# --- multilib: Orthox が使う 2 ABI ぶんを生成 ----------------------------
MT="$DST/gcc/config/riscv/t-linux-multilib"
if [ ! -f "$MT" ]; then
  echo "--- t-linux-multilib を生成 (rv64gc: lp64d / lp64)"
  python3 "$DST/gcc/config/riscv/multilib-generator" rv64gc-lp64d-- rv64gc-lp64-- > "$MT"
fi

echo
echo "=== 移植完了。$DST ==="
