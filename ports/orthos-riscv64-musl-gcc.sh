#!/bin/bash
# riscv64-linux-musl 静的リンク用コンパイラドライバ (orthos-musl-gcc.sh の riscv64 版)
# clang + ports/musl-install-riscv64 sysroot + riscv64 gcc の libgcc を使う。
#
# 以前は既定が homebrew のパス直書きで、Linux では
#   ports/orthos-riscv64-musl-gcc.sh: /opt/homebrew/bin/riscv64-elf-gcc: No such file
# と落ちていた (日報2026-08-03)。環境変数で回避していたが、
# ports/cross-riscv64/ がリポジトリに入ったので、そちらを既定にする。
# homebrew は macOS 向けの最後の候補として残す。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL_ROOT="$ROOT/ports/musl-install-riscv64"
INCLUDEDIR="$MUSL_ROOT/include"
LIBDIR="$MUSL_ROOT/lib"

# 候補を順に見て、最初に使えたものを返す。絶対パスと PATH 上の名前の両方を受ける
pick_tool() {
    local c p
    for c in "$@"; do
        [ -n "$c" ] || continue
        if [ -x "$c" ]; then echo "$c"; return 0; fi
        p="$(command -v "$c" 2>/dev/null || true)"
        if [ -n "$p" ]; then echo "$p"; return 0; fi
    done
    return 1
}

# clang は homebrew LLVM を PATH の clang より先に見る。
# macOS で PATH を先にすると Apple clang を拾ってしまい、riscv64 ターゲットが
# 無くて落ちる。/opt/homebrew は macOS にしか無いので、Linux では素通りする
if ! CLANG_BIN="$(pick_tool "${ORTHOS_RISCV64_CLANG:-}" \
        /opt/homebrew/opt/llvm/bin/clang clang)"; then
    echo "error: clang が見つからない。ORTHOS_RISCV64_CLANG で指定すること" >&2
    exit 1
fi

# riscv64 の gcc は libgcc.a の在処を引くためだけに使う
if ! RVGCC_BIN="$(pick_tool "${ORTHOS_RISCV64_GCC:-}" \
        "$ROOT/ports/cross-riscv64/bin/riscv64-linux-musl-gcc" \
        riscv64-linux-musl-gcc riscv64-elf-gcc \
        /opt/homebrew/bin/riscv64-elf-gcc)"; then
    echo "error: riscv64 の gcc が見つからない (libgcc.a の在処を引くのに要る)。" >&2
    echo "       ports/build_gcc474_riscv_stage2.sh を通すか ORTHOS_RISCV64_GCC で指定すること" >&2
    exit 1
fi

LIBGCC="$("$RVGCC_BIN" -march=rv64gc -mabi=lp64d -print-libgcc-file-name)"
LIBGCC_DIR="$(dirname "$LIBGCC")"

raw_args=("$@")
args=()
compile_only=false
reloc_link=false

for arg in "${raw_args[@]}"; do
    case "$arg" in
        -c|-E|-S)
            compile_only=true
            ;;
        -r|-Wl,-r)
            reloc_link=true
            ;;
        -lc|-lz|-lgcc_s|-lpthread|-ldl|-lrt|*/libz.so*|*/libgcc_s.so*)
            continue
            ;;
        */crt0.o|*/crti.o|*/crtn.o|*/crtbegin*.o|*/crtend*.o|*/rcrt1.o)
            continue
            ;;
    esac
    args+=("$arg")
done

cmd=(
    "$CLANG_BIN"
    --target=riscv64-linux-musl
    -march=rv64gc
    -mabi=lp64d
    -static
    -fno-PIC
    -fno-PIE
    -D__ORTHOS__
    -nostdinc
    -I"$INCLUDEDIR"
    -L"$LIBDIR"
    -L"$LIBGCC_DIR"
)

if ! $compile_only && ! $reloc_link; then
    cmd+=(
        -fuse-ld=lld
        -nostdlib
        -Wl,-Ttext,0x400000
        "$MUSL_ROOT/lib/crt1.o"
        "$MUSL_ROOT/lib/crti.o"
        "${args[@]}"
        -Wl,--start-group
        -lc
        -lm
        "$LIBGCC"
        -Wl,--end-group
        "$MUSL_ROOT/lib/crtn.o"
    )
elif $reloc_link; then
    cmd+=(-fuse-ld=lld -nostdlib "${args[@]}")
else
    cmd+=("${args[@]}")
fi

exec "${cmd[@]}"
