#!/bin/bash
# **rootfs (xv6fs) を SD に焼く前に、カーネル/リポジトリのソースとずれて
# いないか確かめる (S-12)。**
#
# 焼いたイメージに残っている「ビルド時点の git commit」を、いまの
# リポジトリの HEAD と突き合わせる。焼いた後にソースを直すと必ず
# ここでずれる — 気付かずに古い rootfs を dd し続けるのを防ぐための台本。
#
# 刻印を書く側は scripts/build_pi4_rootfs.sh と
# scripts/build_rootfs_aarch64_selfhost.sh (/etc/orthox-build-info)。
# 古いイメージ (刻印を持たない) は「照合できない」として警告だけで通す。
#
#   scripts/verify_rootfs_source.sh out/pi4-rootfs.img
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:?usage: verify_rootfs_source.sh IMG}"
[ -f "$IMG" ] || { echo "*** 無い: $IMG" >&2; exit 1; }

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT

if ! python3 "$ROOT/scripts/build_rootfs_xv6fs.py" --extract /etc/orthox-build-info "$TMP" "$IMG" >/dev/null 2>&1; then
    echo "*** $IMG に /etc/orthox-build-info が無い (刻印より古いイメージ)。照合できない" >&2
    exit 2
fi

IMG_COMMIT="$(grep -m1 '^commit=' "$TMP" | cut -d= -f2-)"
IMG_DIRTY="$(grep -m1 '^dirty=' "$TMP" | cut -d= -f2-)"
IMG_BUILT_AT="$(grep -m1 '^built_at=' "$TMP" | cut -d= -f2-)"

HEAD_COMMIT="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
HEAD_DIRTY="$([ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] && echo 1 || echo 0)"

echo "=== $IMG ==="
echo "  焼いた時点   : commit=$IMG_COMMIT dirty=$IMG_DIRTY built_at=$IMG_BUILT_AT"
echo "  いまの HEAD  : commit=$HEAD_COMMIT dirty=$HEAD_DIRTY"
echo

fail=0

if [ "$IMG_COMMIT" != "$HEAD_COMMIT" ]; then
    echo "*** ずれている: rootfs は $IMG_COMMIT のソースで焼かれたが、HEAD は $HEAD_COMMIT" >&2
    fail=1
fi

if [ "$IMG_DIRTY" != "0" ]; then
    echo "*** 焼いた時点で作業木が未コミットだった (commit だけでは照合しきれない)" >&2
    fail=1
fi

if [ "$HEAD_DIRTY" != "0" ]; then
    echo "*** いまの作業木が未コミット。commit が一致していても中身が違う可能性がある" >&2
    fail=1
fi

echo
if [ "$fail" -ne 0 ]; then
    echo "*** rootfs を焼き直してから dd すること" >&2
    exit 1
fi
echo "ずれ無し。dd してよい"
