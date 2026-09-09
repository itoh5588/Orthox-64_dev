#!/usr/bin/env bash
# G-1: GCC の configure が実機で止まる件の切り分け台本。
#
# 日報2026-08-29 §26 で、3 回目の configure が 74 分以上ハングした。
# **そのとき分かっていたのは「止まった」ことだけで、どこでかは分からなかった。**
# stdout を conf.log (SD 上) に落としていたので、シリアルには何も出ず、
# 生死の判断が [cpu]/[sd] の間接証拠しか無かった。
#
# この台本が変えるのは 3 つ。
#
#   1. **"checking ..." をシリアルに生で流す。**止まった瞬間の最後の 1 行が
#      そのまま止まった場所になる。conf.log を後から読む必要が無い
#   2. **段に割る。**--no-create で「検査」だけを回し、config.status を
#      別に回す。下位ディレクトリも 1 つずつ回せる
#   3. **作業場を /tmp (RAM) に置ける。**SD を一切使わずに同じ検査を回せば、
#      SD が主犯かどうかが 1 回で分かる
#
# ---- 読み取りについて -----------------------------------------------------
#
# **自分では tty を読まない。**dev_up.sh が立てている常時キャプチャと
# 同じ tty を 2 つで開くと、バイトがランダムに分かれて両方壊れる。
# ここではキャプチャのログを**バイトオフセット指定で**読む。
#
# ---- 完了の検出について ---------------------------------------------------
#
# 日報2026-08-29 は完了の誤検出を 3 回踏んでいる。原因は全部同じで、
# **ログ全体を見ていたこと**と、**打った文字のエコーに一致したこと**。
#
#   誤検出 1: `NATIVE-RC=$?` という打った文字そのものに一致した
#   誤検出 2: ログの前のほうにある古いエラー行に一致した
#   誤検出 3: 古い実行の `CONF-RC=127` に一致した
#
# ここでは両方を塞ぐ:
#   - 送信の直前にログのサイズを記録し、**それ以降だけ**を見る
#   - 印は毎回ちがう合言葉つきで、**`RC=` の後ろが数字**のものだけを拾う。
#     打った行のエコーは `RC=$?` のままなので、絶対に一致しない
#
# ---- 使い方 ---------------------------------------------------------------
#
#   bash scripts/pi4/g1_configure.sh env         足場が健全か見るだけ
#   bash scripts/pi4/g1_configure.sh prep        /gb を作り直して CF を置く
#   bash scripts/pi4/g1_configure.sh checks      検査だけ (config.status を回さない)
#   bash scripts/pi4/g1_configure.sh status      config.status だけ
#   bash scripts/pi4/g1_configure.sh full        通しで (従来と同じ)
#   bash scripts/pi4/g1_configure.sh sub libiberty   下位を 1 つだけ
#
#   G1_RAMDIR=1  作業場を /tmp (RAM) に置く。**SD を主犯から外せるか見る**
#   G1_TRACE=1   sh -x で回す。**行単位で位置が出るが、出力量は跳ね上がる**
#   G1_DRYRUN=1  送らずに、送る内容を表示するだけ (実機が要らない)
#   G1_LIMIT=秒  1 段の待ち上限 (既定 3600)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO"

PORT="${PI4_TTY:-/dev/ttyUSB0}"
LIMIT="${G1_LIMIT:-3600}"
DRYRUN="${G1_DRYRUN:-0}"
RAMDIR="${G1_RAMDIR:-0}"
TRACE="${G1_TRACE:-0}"
RUNID="$(date +%H%M%S)$$"
SEQ=0

# 実機側の置き場。**RAM に置くと SD を一切使わずに同じ検査ができる**
if [ "$RAMDIR" = "1" ]; then
    BUILDDIR=/tmp/g1
else
    BUILDDIR=/gb
fi
SRCDIR="${G1_SRCDIR:-/src/gcc-4.7.4}"

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
warn() { printf '  \033[33m--\033[0m   %s\n' "$1"; }
ng()   { printf '  \033[31mNG\033[0m   %s\n' "$1"; }

# --- キャプチャのログを見つける --------------------------------------------
#
# **dev_up.sh が立てたものを使う。**自分で cat を足さない
CAPTURE=""
find_capture() {
    local p
    for p in $(pgrep -x cat 2>/dev/null); do
        if tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | grep -qx -- "$PORT"; then
            # cat の stdout が向いている先がキャプチャのログ
            CAPTURE="$(readlink -f "/proc/$p/fd/1" 2>/dev/null)"
            [ -n "$CAPTURE" ] && return 0
        fi
    done
    return 1
}

# --- 送受信 ----------------------------------------------------------------

# オフセット以降の受信。カーネルの定期出力は既定で落とす
region() {   # $1 = 開始オフセット, $2 = "raw" なら落とさない
    local from="$1"
    if [ "${2:-}" = "raw" ]; then
        tail -c "+$(( from + 1 ))" "$CAPTURE" 2>/dev/null | tr -d '\r'
    else
        tail -c "+$(( from + 1 ))" "$CAPTURE" 2>/dev/null | tr -d '\r' \
            | grep -av '^\[usb\]\|^\[cpu\]\|^\[sd\]'
    fi
}

# 区間の文字列から印を拾う。**`RC=` の後ろが数字のものだけ。**
#
# 日報2026-08-29 の誤検出 1 は、`NATIVE-RC=$?` という**打った文字そのもの**に
# 一致したものだった。ash は打った行をエコーで返すので、区間には必ず
# `RC=$?` が含まれる。数字だけを見れば、これには当たらない。
# 合言葉が毎回ちがうことで、誤検出 2/3 (古い実行の印に当たる) も塞がる。
#
# 印が無ければ何も出さない (空文字列)
marker_rc() {   # $1 = 合言葉, stdin = 区間
    grep -aoE "<<$1_RC=[0-9]+>>" | head -1 | sed 's/.*RC=//; s/>>//'
}

# 1 つ送って、印が返るまで待つ。
#
# **印は毎回ちがう合言葉つきで、`RC=` の後ろが数字のものだけを拾う。**
# 打った行はエコーで戻ってくるが、そちらは `RC=$?` のままなので当たらない
send() {   # $1 = コマンド, $2 = 待ち上限秒 (任意), $3 = 見出し (任意)
    local cmd="$1" limit="${2:-$LIMIT}" title="${3:-}"
    SEQ=$(( SEQ + 1 ))
    local tag="G1_${RUNID}_${SEQ}"
    local line="$cmd; echo \"<<${tag}_RC=\$?>>\""
    local before elapsed=0 shown=0 quiet=0 rc

    [ -n "$title" ] && echo "=== $title ==="
    if [ "$DRYRUN" = "1" ]; then
        printf '  送る: %s\n' "$line"
        return 0
    fi

    # 印そのものを除いた本文。数える側と出す側で同じものを見る
    body() { region "$before" | grep -av "<<${tag}_RC="; }

    before="$(stat -c %s "$CAPTURE")"
    printf '%s\r' "$line" > "$PORT"

    while [ "$elapsed" -lt "$limit" ]; do
        # **`RC=` の後ろが数字のものだけ。**打った行のエコー (`RC=$?`) は落ちる
        rc="$(region "$before" | marker_rc "$tag")"
        if [ -n "$rc" ]; then
            body | tail -n +$(( shown + 1 ))
            if [ "$rc" = "0" ]; then ok "RC=0  (${elapsed}s)"; else ng "RC=$rc  (${elapsed}s)"; fi
            return "$rc"
        fi
        # 届いたぶんを流す。**これが生きている証拠になる。**
        # 止まったときの最後の 1 行が、そのまま止まった場所
        local now
        now="$(body | wc -l)"
        if [ "$now" -gt "$shown" ]; then
            body | tail -n +$(( shown + 1 ))
            shown="$now"
            quiet=0
        else
            quiet=$(( quiet + 2 ))
            # **黙っていることを成功と見ない。**60 秒に 1 回、
            # 経過とカーネル側の様子を出す。ここが空なら本当に死んでいる
            if [ "$quiet" -ge 60 ]; then
                printf '  ... %d 秒経過、出力なし。カーネル側:\n' "$elapsed"
                region "$before" raw | grep -a '^\[cpu\]' | tail -1 | sed 's/^/       /'
                region "$before" raw | grep -a '^\[sd\]'  | tail -1 | sed 's/^/       /'
                quiet=0
            fi
        fi
        sleep 2
        elapsed=$(( elapsed + 2 ))
    done

    ng "${limit} 秒待っても印が返らない (合言葉 ${tag})"
    echo "*** **これがハングの現場。**最後に出た 20 行:" >&2
    body | tail -20 | sed 's/^/***   /' >&2
    echo "*** カーネルが生きているかは次で見る (この区間の [cpu]/[sd]):" >&2
    region "$before" raw | grep -a '^\[cpu\]\|^\[sd\]' | tail -4 | sed 's/^/***   /' >&2
    return 124
}

# --- 実機側に置く設定 -------------------------------------------------------
#
# 日報2026-08-29 §26 の 3 回目と同じもの。**変えたのは置き場だけ**
cf_lines() {
    cat <<'EOS'
CF="--host=aarch64-linux-musl --build=aarch64-linux-musl"
CF="$CF --target=aarch64-linux-musl --prefix=/usr/local"
CF="$CF --enable-languages=c --disable-bootstrap --disable-multilib"
CF="$CF --disable-nls --disable-shared --disable-libssp --disable-libquadmath"
CF="$CF --disable-libgomp --disable-libmudflap --disable-lto --disable-werror"
CF="$CF --with-gmp=/usr --with-mpfr=/usr --with-mpc=/usr"
export CC=/usr/bin/gcc AR=/usr/bin/ar RANLIB=/usr/bin/ranlib
export CC_FOR_BUILD=/usr/bin/gcc CC_FOR_TARGET=/usr/bin/gcc
export AS=/usr/bin/as LD=/usr/bin/ld NM=/usr/bin/nm
EOS
}

# **既定では configure を直に叩く。**日報2026-08-29 §26 と同じ形にして、
# 変わるのが「stdout の行き先」と「段の割り方」だけになるようにする
# (直に叩く経路は #! の実装を通る。そこも含めて再現したい)。
# G1_TRACE=1 のときだけ sh -x を噛ませる
SHRUN=""
[ "$TRACE" = "1" ] && SHRUN="sh -x"

# --- 段 ---------------------------------------------------------------------

stage_env() {
    echo "--- 足場の健全性 ---"
    send 'echo alive' 20 'プロンプトが返るか'
    send 'busybox ls -l /usr/bin/gcc' 30 'gcc が居るか'
    send 'busybox find / -name cc1 2>/dev/null | busybox head -3' 120 'cc1 の在り処'
    send "busybox ls -d $SRCDIR/configure" 30 'GCC のソースが載っているか'
    # **P-7 の確認も兼ねる。**空の ramfs ディレクトリが ls できない件
    send 'rm -rf /tmp/g1probe; busybox mkdir -p /tmp/g1probe; busybox ls -a /tmp/g1probe' 30 \
         '/tmp (RAM) が使えるか  ※空だと ls が落ちるのが P-7'
    send 'echo x > /tmp/g1probe/x; busybox cat /tmp/g1probe/x; rm -rf /tmp/g1probe' 30 \
         '/tmp に読み書きできるか'
    send 'busybox df / /tmp 2>/dev/null || echo df-なし' 30 '空き容量'
}

stage_prep() {
    echo "--- 作業場を作る  (置き場: $BUILDDIR) ---"
    send "cd /; rm -rf $BUILDDIR; busybox mkdir -p $BUILDDIR; cd $BUILDDIR; pwd" 120 '作り直す'
    local l
    while IFS= read -r l; do
        [ -z "$l" ] && continue
        send "$l" 20 || { ng "ここで止める。以降の段は当てにならない"; return 1; }
    done < <(cf_lines)
    send 'echo "CF=[$CF]"' 20 'CF の確認'
}

# **検査だけ。config.status を回さない。**
#
# --no-create (-n) は autoconf の標準の選択肢。**全部の checking を回し、
# config.status を書いたうえで、それを実行せずに終わる**
# (gcc-4.7.4 の configure:15666 `if test "$no_create" != yes` で確認した)。
# 「検査で止まる」のか「ファイル生成で止まる」のかが、これで割れる。
# checks が通れば、続けて status を回せる (config.status は既に在る)。
#
# **stdout をシリアルに出す。**日報2026-08-29 はここを conf.log に落として
# いたので、止まった位置が見えなかった
stage_checks() {
    echo "--- 検査だけ (--no-create)  置き場: $BUILDDIR ---"
    echo "    checking ... が生で流れる。**最後の 1 行が止まった場所**"
    send "cd $BUILDDIR && $SHRUN $SRCDIR/configure \$CF --no-create 2>&1" \
         "$LIMIT" 'configure --no-create'
}

stage_status() {
    echo "--- config.status だけ  置き場: $BUILDDIR ---"
    send "cd $BUILDDIR && busybox ls -l config.status" 60 'config.status が在るか'
    send "cd $BUILDDIR && $SHRUN ./config.status 2>&1" "$LIMIT" 'config.status'
}

stage_full() {
    echo "--- 通しで  置き場: $BUILDDIR ---"
    send "cd $BUILDDIR && $SHRUN $SRCDIR/configure \$CF 2>&1" "$LIMIT" 'configure (通し)'
}

# **下位には CF をそのまま渡さない。**--enable-languages のような
# トップレベル専用の選択肢は下位が知らず、"unrecognized options" の警告で
# 出力が埋まる。切り分けに要るのは三つ組と prefix だけ
stage_sub() {
    local dir="$1"
    local subcf='--host=aarch64-linux-musl --build=aarch64-linux-musl --target=aarch64-linux-musl --prefix=/usr/local'
    echo "--- 下位ディレクトリ 1 つだけ: $dir ---"
    send "busybox ls -d $SRCDIR/$dir/configure" 30 'その configure が在るか'
    send "cd /; rm -rf $BUILDDIR/$dir; busybox mkdir -p $BUILDDIR/$dir" 60 '作り直す'
    send "cd $BUILDDIR/$dir && $SHRUN $SRCDIR/$dir/configure $subcf 2>&1" \
         "$LIMIT" "configure ($dir)"
}

# --- 入口 -------------------------------------------------------------------

# 試験から関数だけ読むための口 (tests/pi4_g1_marker_test.sh)
if [ "${G1_LIB_ONLY:-0}" = "1" ]; then
    return 0 2>/dev/null || exit 0
fi

# **接続の確認はここで。**dev_up.sh のキャプチャに相乗りする
if [ "$DRYRUN" != "1" ]; then
    if [ ! -c "$PORT" ]; then
        ng "$PORT が無い。先に bash scripts/pi4/dev_up.sh を回す"
        exit 1
    fi
    if ! find_capture; then
        ng "$PORT のキャプチャが立っていない"
        echo "     bash scripts/pi4/dev_up.sh  を先に回すこと。" >&2
        echo "     **ここで自分の cat を立ててはいけない。**同じ tty を 2 つで" >&2
        echo "     開くとバイトが両者に分かれ、どちらのログも壊れる。" >&2
        exit 1
    fi
    ok "キャプチャを使う: $CAPTURE"
fi

case "${1:-}" in
    env)    stage_env ;;
    prep)   stage_prep ;;
    checks) stage_checks ;;
    status) stage_status ;;
    full)   stage_full ;;
    sub)    [ -n "${2:-}" ] || { ng "sub にはディレクトリ名が要る (例: sub libiberty)"; exit 1; }
            stage_sub "$2" ;;
    *)
        echo "使い方: bash scripts/pi4/g1_configure.sh {env|prep|checks|status|full|sub <dir>}"
        echo
        echo "  env     足場が健全か見るだけ (壊さない)"
        echo "  prep    作業場を作り直して CF を置く"
        echo "  checks  検査だけ。--no-create で config.status を回さない"
        echo "  status  config.status だけ"
        echo "  full    通しで (従来と同じ)"
        echo "  sub D   下位ディレクトリ D の configure だけ"
        echo
        echo "  G1_RAMDIR=1  作業場を /tmp (RAM) へ。**SD を主犯から外せるか見る**"
        echo "  G1_TRACE=1   sh -x で回す"
        echo "  G1_DRYRUN=1  送らずに表示するだけ (実機が要らない)"
        echo "  G1_LIMIT=秒  1 段の待ち上限 (既定 3600)"
        exit 1 ;;
esac
