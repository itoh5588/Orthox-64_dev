#!/bin/bash
# Orthox の riscv64 カーネルを GCC 4.6.4 (RISC-V フォーク) で通してみて、
# 何がどれだけ壊れるかを数える。
#
# 「4.7.4 へ前方移植する」と「4.6.4 のまま __sync_* に寄せる」の判断材料。
# コンパイルのみ (-c)。リンクはしない。
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 第 1 引数で対象ツリーを選ぶ。既定は 4.7.4 移植版 (本命)。
#   ./try_kernel_riscv.sh                  → gcc-4.7.4-riscv (27/27 が期待値)
#   ./try_kernel_riscv.sh gcc-4.6.4-riscv  → 移植前のフォーク (24/27。__atomic_* で 3 件 NG)
TREE="${1:-gcc-4.7.4-riscv}"
BDIR="$ROOT/ports/$TREE/build-riscv64/gcc"
XGCC="$BDIR/xgcc"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

[ -x "$XGCC" ] || { echo "error: $XGCC が無い"; exit 1; }

# Makefile の RISCV64_CFLAGS から clang 固有のものを除いたもの。
# --target=      … clang 専用 (gcc は -dumpmachine が既に riscv64)
# -fno-PIE       … 4.6.4 に無い綴り。4.6.4 の既定は非 PIC なので不要
# -fno-stack-check/-fno-lto は 4.6.4 にもある
#
# -std=gnu1x が要る。Makefile は -std= を渡していないが clang の既定が gnu17 なので
# C99 以降として通っている。4.6.4 の既定は C89 で、これを渡さないと
# 「'for' loop initial declarations are only allowed in C99 mode」が 188 件出て
# 本当の非互換が埋もれる。4.6.4 に -std=c11 は無く c1x/gnu1x が同等。
CFLAGS="-std=gnu1x -march=rv64gc -mabi=lp64 -ffreestanding -fno-stack-protector"
CFLAGS="$CFLAGS -fno-stack-check -fno-lto -mcmodel=medany -O2 -Wall -Wextra -I$ROOT/include"

SRCS="
kernel/riscv64/boot.c kernel/riscv64/bootstrap_user.c kernel/riscv64/elf.c
kernel/riscv64/entry.c kernel/riscv64/fs.c kernel/riscv64/net_socket.c
kernel/riscv64/plic.c kernel/riscv64/pmm.c kernel/riscv64/runtime.c
kernel/riscv64/smp.c kernel/riscv64/task.c kernel/riscv64/trap.c
kernel/riscv64/syscall.c kernel/riscv64/virtio_blk_mmio.c kernel/riscv64/vm.c
kernel/task.c kernel/task_exec.c kernel/task_fork.c kernel/sched.c
kernel/wait.c kernel/elf.c kernel/cstring.c kernel/cstdio.c
kernel/storage.c kernel/xv6bio.c kernel/xv6log.c kernel/xv6fs.c
"

nok=0; nng=0
declare -a FAILED

echo "=== $("$XGCC" -B"$BDIR" --version | head -1) で Orthox riscv64 カーネルを試す ==="
echo "CFLAGS: $CFLAGS"
echo

for s in $SRCS; do
  log="$OUT/$(echo "$s" | tr / _).log"
  if "$XGCC" -B"$BDIR" $CFLAGS -c "$ROOT/$s" -o "$OUT/o.o" 2>"$log"; then
    printf "  OK   %s\n" "$s"
    nok=$((nok+1))
  else
    printf "  NG   %s\n" "$s"
    nng=$((nng+1))
    FAILED+=("$s|$log")
  fi
done

echo
echo "=== 集計: OK $nok / NG $nng ==="
echo

if [ "$nng" -gt 0 ]; then
  echo "=== 失敗の原因を分類 ==="
  : > "$OUT/all.err"
  for e in "${FAILED[@]}"; do cat "${e#*|}" >> "$OUT/all.err"; done
  # error: 行だけを見て、原因の種類ごとに数える
  grep -h "error:" "$OUT/all.err" \
    | sed -E "s/.*error: //" \
    | sed -E "s/'__atomic_[a-z_]+'/'__atomic_*'/" \
    | sort | uniq -c | sort -rn | head -20 | sed 's/^/  /'
  echo
  echo "=== ファイルごとの最初の error ==="
  for e in "${FAILED[@]}"; do
    printf "  %-34s %s\n" "${e%%|*}" "$(grep -m1 'error:' "${e#*|}" | sed -E 's/.*error: //' | cut -c1-60)"
  done
fi
