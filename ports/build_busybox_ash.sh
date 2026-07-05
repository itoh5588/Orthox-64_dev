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

export AR="$(find_tool x86_64-elf-ar x86_64-linux-gnu-ar ar)"
export RANLIB="$(find_tool x86_64-elf-ranlib x86_64-linux-gnu-ranlib ranlib)"
export STRIP="$(find_tool x86_64-elf-strip x86_64-linux-gnu-strip strip)"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$ROOT/user/include}"
if [ "$BUILD_DYNAMIC" = "1" ]; then
  export CFLAGS=""
  BUSYBOX_EXTRA_CFLAGS="-O2 -fPIE -fno-strict-aliasing -D__ORTHOS__ -D_GNU_SOURCE -DORTHOX_BUSYBOX_ASH_PTR_HACK=1 -DORTHOX_BUSYBOX_TEST_PTR_HACK=1 -Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
  BUSYBOX_EXTRA_LDFLAGS=""
else
  export CFLAGS=""
  BUSYBOX_EXTRA_CFLAGS="-O2 -I$INCLUDEDIR -Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
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
