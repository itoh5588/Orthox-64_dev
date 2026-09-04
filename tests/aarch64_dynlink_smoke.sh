#!/bin/bash
# aarch64 の動的リンク一式を Orthox の上で走らせて判定する。
#
# **「組めた」と「動いた」は別。**共有 musl を作っただけでは、
#   - 動的リンカが読み込まれるか
#   - .so を開けるか (file-backed mmap と MAP_FIXED)
#   - .so のテキストが実行できるか (PROT_EXEC)
#   - .so ごとの TLS が別々に効くか
#   - dlopen / dlsym が通るか
# は分からない。ここまで見て初めて使えると言える。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
[ -n "$QEMU_BIN" ] || { echo "qemu-system-aarch64 not found" >&2; exit 1; }

SR=ports/musl-install-aarch64
DISK=out/aarch64-dyn-disk.img
FS=out/aarch64-dyn-fs
O=out/aarch64-dyn
LOGDIR=out/aarch64-dyn-logs

[ -f "$SR/lib/libc.so" ] || {
  echo "★ $SR/lib/libc.so が無い。make $SR/lib/libc.a (--enable-shared) が先" >&2
  exit 1; }

bash scripts/build_aarch64_dynlink.sh >/dev/null

# ---- 試験用ディスク -------------------------------------------------------
# **xv6fs に symlink 型は無い。**ld-musl-aarch64.so.1 は実体で置く
rm -rf "$FS"; mkdir -p "$FS/bin" "$FS/lib"
cp "$O"/*.elf "$FS/bin/"
cp "$O"/*.so  "$FS/lib/"
cp "$SR/lib/libc.so" "$FS/lib/libc.so"
cp "$SR/lib/libc.so" "$FS/lib/ld-musl-aarch64.so.1"
# カーネルの fs 自己診断が中身まで照合する既知ファイル
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$FS/aarch64-m4.txt"
rm -f "$DISK"
XV6FS_FSSIZE=16384 XV6FS_NINODES=256 \
  python3 scripts/build_rootfs_xv6fs.py "$FS" "$DISK" >/dev/null

mkdir -p "$LOGDIR"
fail=0

run_one() { # run_one <プログラム名> <期待する行>
  local prog="$1" want="$2" log="$LOGDIR/$1.log"
  make out/kernel-aarch64.elf AARCH64_INIT_PATH_VALUE="/bin/$prog.elf" >/dev/null 2>&1
  timeout 90 "$QEMU_BIN" -machine virt,virtualization=on -cpu cortex-a72 \
    -m 512M -smp 1 -nographic -snapshot -kernel out/kernel-aarch64.elf \
    -drive file="$DISK",if=none,format=raw,id=vblk0 \
    -device virtio-blk-device,drive=vblk0 > "$log" 2>&1 || true

  if grep -aqF -- "$want" "$log"; then
    printf '  %-22s ok   (%s)\n' "$prog" "$want"
  else
    printf '  %-22s ★ NG (期待: %s)\n' "$prog" "$want"
    # **落ちた理由を出す。**動的リンカの文言はここでしか見えない
    grep -aE "Error (loading|relocating)|ENOSYS|unexpected exception|FAIL" "$log" | head -4 | sed 's/^/       /'
    fail=1
  fi
  # **未対応 syscall を見逃さない。**動いてしまうので気づけない
  if grep -aq "ENOSYS" "$log"; then
    printf '       (注) ENOSYS: %s\n' "$(grep -ao 'ENOSYS: syscall [0-9a-f]*' "$log" | sort -u | tr '\n' ' ')"
  fi
}

echo "--- aarch64 dynlink smoke ---"
run_one hello_dyn         "Hello, Orthox-64 with Shared Library!"
run_one dynlink_malloc    "dynlink-malloc: PASS"
run_one dynlink_multi_tls "dynlink-multi-tls: PASS"
run_one dynlink_dlopen    "dynlink-dlopen: PASS"

echo "--------------------------------"
if [ "$fail" = 0 ]; then
  echo "aarch64 dynlink smoke test: PASS"
else
  echo "aarch64 dynlink smoke test: FAIL"
  exit 1
fi
