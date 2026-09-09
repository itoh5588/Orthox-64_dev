#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/ports/Python-3.12.3"
INSTALL_DIR="$ROOT/ports/python-install"

cd "$BUILD_DIR"

# Orthox-64 (musl) 向けの設定
export CONFIG_SITE=config.site
cat <<EOF > config.site
ac_cv_file__dev_ptmx=no
ac_cv_file__dev_ptc=no
# musl で不足しているか、Orthox カーネルで未実装の可能性が高いもの
ac_cv_func_fanotify_init=no
ac_cv_func_fanotify_mark=no
ac_cv_func_inotify_init=no
ac_cv_func_inotify_add_watch=no
ac_cv_func_inotify_rm_watch=no
ac_cv_func_inotify_init1=no
ac_cv_func_prlimit=no
EOF

# 一度ビルド環境をクリーンアップ
make clean || true

./configure \
    --host=x86_64-pc-linux-musl \
    --build=x86_64-pc-linux-gnu \
    --disable-ipv6 \
    --with-pydebug \
    --without-ensurepip \
    --with-build-python=python3 \
    MACHDEP=linux \
    CC="$ROOT/ports/orthos-musl-gcc-dyn.sh" \
    CXX="$ROOT/ports/orthos-musl-gcc-dyn.sh" \
    CFLAGS="-D__ORTHOS__ -D_GNU_SOURCE -D_Py_FORCE_UTF8_FS_ENCODING -D_Py_FORCE_UTF8_LOCALE" \
    LDFLAGS="-Wl,-E" \
    LIBS="-lc -lm" \
    --prefix="/"
