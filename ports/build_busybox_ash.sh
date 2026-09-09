#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/ports/busybox}"
OUT="${2:-$ROOT/user/busybox-ash.elf}"
CONFIG_FILE="${ORTHOS_BUSYBOX_CONFIG:-$ROOT/ports/busybox-ash.config}"
BUILD_DYNAMIC="${ORTHOS_BUSYBOX_DYNAMIC:-0}"
case "$OUT" in
  /*) ;;
  *) OUT="$ROOT/$OUT" ;;
esac

if [ ! -d "$SRC" ]; then
  echo "busybox source not found: $SRC" >&2
  exit 1
fi

# Orthox 向けの busybox 改変を当てる。
#
# ports/busybox は .gitignore 対象なので、clone した直後の木には当たっていない。
# 以前はこのパッチがどこからも適用されておらず「手で当てる」運用だった (当て
# 忘れると Clang が ash / lineedit の const ポインタハックを畳み込んで、起動
# 直後にページフォルトする)。ビルドの入口で必ず通るここに置く。
#
# 冪等性: 逆向きに当たるなら適用済みとみなして何もしない。順方向に当たるなら
# 当てる。どちらでもない (部分適用など) なら黙って進まず落とす。
# 判定はすべて --dry-run で行うので .rej / .orig を残さない。
PATCH_FILE="${ORTHOS_BUSYBOX_PATCH:-$ROOT/ports/busybox-orthox.patch}"
if [ -f "$PATCH_FILE" ]; then
  if (cd "$SRC" && patch -p1 --dry-run -R -f -s < "$PATCH_FILE" >/dev/null 2>&1); then
    echo "busybox: Orthox パッチは適用済み ($PATCH_FILE)"
  elif (cd "$SRC" && patch -p1 --dry-run -f -s < "$PATCH_FILE" >/dev/null 2>&1); then
    (cd "$SRC" && patch -p1 -f -s < "$PATCH_FILE")
    echo "busybox: Orthox パッチを適用した ($PATCH_FILE)"
  else
    echo "busybox: Orthox パッチを適用できない: $PATCH_FILE" >&2
    echo "  適用済みでも未適用でもない (部分適用 / busybox のバージョン違い) 可能性がある。" >&2
    echo "  確認: cd $SRC && patch -p1 --dry-run < $PATCH_FILE" >&2
    echo "  git 管理下なら復旧: git -C $SRC checkout ." >&2
    echo "  (自動では直さない。busybox 側の手作業を消してしまうため)" >&2
    exit 1
  fi
else
  echo "busybox: パッチが見つからない: $PATCH_FILE" >&2
  exit 1
fi

export PATH="/opt/homebrew/bin:$PATH"
export LC_ALL=C

if [ -n "${ORTHOS_CC:-}" ]; then
  export CC="$ORTHOS_CC"
elif [ "$BUILD_DYNAMIC" = "1" ]; then
  export CC="${ORTHOS_BUSYBOX_DYNAMIC_CC:-$ROOT/ports/orthos-musl-gcc-dyn.sh}"
elif [[ "${ORTHOS_SYSROOT:-}" == *"/musl-install" ]]; then
  export CC="$ROOT/ports/orthos-musl-gcc.sh"
else
  export CC="$ROOT/ports/orthos-gcc.sh"
fi

if [ -n "${ORTHOS_LD:-}" ]; then
  export LD="$ORTHOS_LD"
else
  export LD="$CC"
fi

find_tool() {
  for tool in "$@"; do
    if command -v "$tool" >/dev/null 2>&1; then
      command -v "$tool"
      return 0
    fi
  done
  return 1
}

export AR="${ORTHOS_AR:-$(find_tool x86_64-elf-ar x86_64-linux-gnu-ar ar)}"
export RANLIB="${ORTHOS_RANLIB:-$(find_tool x86_64-elf-ranlib x86_64-linux-gnu-ranlib ranlib)}"
export STRIP="${ORTHOS_STRIP:-$(find_tool x86_64-elf-strip x86_64-linux-gnu-strip strip)}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$ROOT/user/include}"
if [ "$BUILD_DYNAMIC" = "1" ]; then
  export CFLAGS=""
  BUSYBOX_EXTRA_CFLAGS="-O2 -fPIE -fno-strict-aliasing -D__ORTHOS__ -D_GNU_SOURCE -DORTHOX_BUSYBOX_ASH_PTR_HACK=1 -DORTHOX_BUSYBOX_TEST_PTR_HACK=1 -DORTHOX_BUSYBOX_LINEEDIT_PTR_HACK=1 -Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
  BUSYBOX_EXTRA_LDFLAGS=""
else
  export CFLAGS=""
  BUSYBOX_EXTRA_CFLAGS="-O2 -I$INCLUDEDIR ${ORTHOS_EXTRA_CFLAGS:-} -Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
  BUSYBOX_EXTRA_LDFLAGS="-static -nostartfiles"
fi
export LDFLAGS="$BUSYBOX_EXTRA_LDFLAGS"
export HOSTCFLAGS="-Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
export LDLIBS=""

cd "$SRC"
rm -f .config
KCONFIG_ALLCONFIG="$CONFIG_FILE" \
make CC="$CC" LD="$LD" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" allnoconfig
make CC="$CC" LD="$LD" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" clean >/dev/null
NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
make CC="$CC" LD="$LD" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    EXTRA_CFLAGS="$BUSYBOX_EXTRA_CFLAGS" EXTRA_LDFLAGS="$BUSYBOX_EXTRA_LDFLAGS" \
    -j"$NCPU" busybox
cp busybox "$OUT"
