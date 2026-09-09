#!/bin/bash
# **SD に載せる前に、そのカーネルが期待どおりのものか確かめる。**
#
# 何度も外した:
#   - フラグを付けたのに**呼び出しを繋いでいなかった**
#   - 作った後に**スモークが同じファイルを既定設定で作り直していた**
#     (CFLAGS の変化で組み直すようにした結果、余計に起きやすくなった)
#
# **strings で「それらしい文字列」を探すだけでは足りない。**
# #ifdef の外にある文字列は既定ビルドにも入っているので、有効・無効の
# 区別がつかない。**有効時にしか存在しない印**を見ること。
#
#   scripts/pi4_verify_image.sh out/pi4-boot/kernel8.img [期待する init パス]
set -euo pipefail

IMG="${1:-out/pi4-boot/kernel8.img}"
WANT_INIT="${2:-}"
[ -f "$IMG" ] || { echo "*** 無い: $IMG" >&2; exit 1; }

echo "=== $IMG ==="
echo "  大きさ : $(stat -c %s "$IMG") バイト"
echo "  md5    : $(md5sum "$IMG" | cut -d' ' -f1)"
echo "  日時   : $(stat -c %y "$IMG" | cut -d. -f1)"
echo

fail=0

# init のパス。**既定は /bin/hello** なので、ash を期待していて hello が
# 出たら「既定ビルドを掴んでいる」と分かる
init_found=$(strings "$IMG" | grep -oE "^/bin/(ash|hello|doom)$" | sort -u | tr '\n' ' ')
echo "  init パス候補 : ${init_found:-(見つからない)}"
# **grep -q を pipefail の下で使わないこと。**
#
# grep -q は最初の一致で終了するので strings に SIGPIPE が飛び、
# set -o pipefail がそれを失敗と見なす。**読みが遅い相手 (SD の drvfs)
# でだけ起きる**ので、ローカルで試すと通ってしまう (実測でそうなった)。
# 数を数える形にする
count_in_image() {   # $1 = パターン
    strings "$IMG" | grep -c -- "$1" || true
}

if [ -n "$WANT_INIT" ]; then
    if [ "$(strings "$IMG" | grep -cx -- "$WANT_INIT" || true)" -gt 0 ]; then
        echo "    -> $WANT_INIT を含む  ok"
    else
        echo "    -> *** $WANT_INIT が無い" >&2
        fail=1
    fi
fi

# **有効時にしか存在しない印。** #ifdef の中にある文字列を選ぶこと
n_init=$(count_in_image "pcie init")
echo "  PCIe 立ち上げ : $n_init 段  ($([ "$n_init" -ge 6 ] && echo 有効 || echo 無効))"

n_probe=$(count_in_image "pcie probe")
echo "  PCIe 探針     : $([ "$n_probe" -gt 0 ] && echo 有効 || echo 無効)"

n_kbd=$(count_in_image "usb-kbd-probe-start")
echo "  USB キー探針  : $([ "$n_kbd" -gt 0 ] && echo 有効 || echo 無効)"

# 常に入っているべきもの (入っていなければビルドが壊れている)
for pat in "pcie xhci :" "dma offset" "HID keyboard if=" "fb console"; do
    if [ "$(count_in_image "$pat")" -gt 0 ]; then
        printf "  %-18s : ok\n" "$pat"
    else
        printf "  %-18s : *** 無い\n" "$pat" >&2
        fail=1
    fi
done

echo
if [ "$fail" -ne 0 ]; then
    echo "*** 期待と違う。載せないこと" >&2
    exit 1
fi
echo "載せてよい"
