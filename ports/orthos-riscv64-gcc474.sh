#!/bin/bash
# 移植版 GCC 4.7.4 (RISC-V バックエンド) を Makefile の RISCV64_CC として使うラッパ。
#
# Makefile は clang 前提で書かれているので、gcc が受け付けない綴りだけ落とす:
#   --target=riscv64-none-elf … clang 専用。gcc は configure 時の target で固定
#   -fno-PIE                  … 4.7.4 に無い綴り (既定が非 PIC なので不要)
# 逆に -std= は Makefile が渡さない (clang の既定が gnu17) ので、こちらで足す。
# 4.7.4 に -std=c11 の綴りは無く c1x/gnu1x が同等。
#
# 使い方:
#   make riscv64-kernel \
#     RISCV64_CC=$(pwd)/ports/orthos-riscv64-gcc474.sh \
#     RISCV64_OBJCOPY=$(pwd)/ports/cross-riscv64/bin/riscv64-linux-musl-objcopy \
#     LD=$(pwd)/ports/cross-riscv64/bin/riscv64-linux-musl-ld
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BDIR="$ROOT/ports/gcc-4.7.4-riscv/build-riscv64/gcc"
XGCC="$BDIR/xgcc"

if [ ! -x "$XGCC" ]; then
  echo "error: $XGCC が無い。ports/build_gcc474_riscv.sh を先に通すこと" >&2
  exit 1
fi

args=()
has_std=false
skip_next=false
for a in "$@"; do
  if [ "$skip_next" = true ]; then skip_next=false; continue; fi
  case "$a" in
    --target=*)  ;;                    # clang 専用
    -target)     skip_next=true ;;     # clang 専用 (2 語形。build_musl.sh が使う)
    -fuse-ld=*)  ;;                    # 4.7.4 に無い。リンカは -B と PATH で決まる
    -fno-PIE)    args+=("-fno-pie") ;; # 4.7.4 の綴り
    -std=*)      has_std=true; args+=("$a") ;;
    *)           args+=("$a") ;;
  esac
done

if [ "$has_std" = false ]; then
  set -- -std=gnu1x "${args[@]}"
else
  set -- "${args[@]}"
fi

exec "$XGCC" -B"$BDIR" "$@"
