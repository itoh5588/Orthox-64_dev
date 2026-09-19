#!/bin/bash
# fork の CoW を 4 hart で叩く (2026-09-19)。riscv64 版。
#
# user/cowstress.c をブートストラップのユーザープログラムとしてカーネルに
# 埋め込み (RISCV64_BOOTSTRAP_USER_SRC_ELF)、QEMU の virt を -smp 4 で起動する。
# worker 8 本がそれぞれ fork を繰り返し、親子が同じページを別の hart で同時に
# 写す。**中身が混ざらないこと** (cowstress: PASS) と、カーネルが止まらない
# ことを見る。aarch64 版は tests/aarch64_cowstress_smoke.sh。
#
#   make riscv64-cowstress-smoke
#   SMP_CPUS で hart 数を変えられる (既定 4)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs

QEMU_BIN="$(command -v qemu-system-riscv64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-riscv64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-riscv64
fi
if [ -z "$QEMU_BIN" ] && [ -x /usr/local/bin/qemu-system-riscv64 ]; then
    QEMU_BIN=/usr/local/bin/qemu-system-riscv64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-riscv64 not found" >&2
    exit 1
fi

FW_PATH=""
if [ -f /opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
elif [ -f /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    FW_PATH=/usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
elif [ -f /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin ]; then
    # Debian/Ubuntu の qemu-system-misc はここに置く
    FW_PATH=/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SERIAL_LOG=LOGs/riscv64-cowstress-serial.log
rm -f "$SERIAL_LOG" "$SERIAL_LOG.nocr"

# rootfs があれば繋ぐ。probe の BIGWRITE (xv6fs のログ分割の退行検査) は
# 書き込める FS が要るので、無い構成では probe 側が静かに飛ばす。
ROOTFS_IMG=out/rootfs-riscv64-xv6.img
DRIVE_ARGS=()
if [ -f "$ROOTFS_IMG" ]; then
    DRIVE_ARGS=(-drive "file=$ROOTFS_IMG,if=none,format=raw,id=vblk0" -device virtio-blk-device,drive=vblk0)
fi

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp "${SMP_CPUS:-4}" \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -monitor none "${DRIVE_ARGS[@]}" &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..300}; do
    if grep -q "bootstrap user exit" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

sleep 1

echo "--- RISC-V cowstress Serial Output ---"
cat "$SERIAL_LOG"
echo "--------------------------------------"

# 判定は CR を除いたコピーに当てる (ユーザーの出力は ONLCR で CRLF になる)
tr -d '\r' < "$SERIAL_LOG" > "$SERIAL_LOG.nocr"
LOG="$SERIAL_LOG.nocr"

must_not() {   # $1 = 固定文字列, $2 = 説明
    if grep -aqF -- "$1" "$LOG"; then
        echo "*** 出てはいけないものが出た: $1" >&2
        echo "*** $2" >&2
        grep -aF -- "$1" "$LOG" | head -3 | sed 's/^/***   /' >&2
        exit 1
    fi
}

if [ "${SMP_CPUS:-4}" -gt 1 ]; then
    grep -aq "\[smp\] cpus online: 0x000000000000000${SMP_CPUS:-4}" "$LOG"
fi
grep -aq "fork selftest passed" "$LOG"
grep -aqE "^cowstress: start workers=" "$LOG"
must_not "cowstress: FAIL" "CoW で親子の中身が混ざった / fork・wait が失敗した"
must_not "cow: no page to copy into" "CoW の写し先のページが取れなかった"
must_not "ENOSYS: syscall" "未実装の syscall を呼んだ"
grep -aqE "^cowstress: PASS workers=" "$LOG"
grep -aq "bootstrap user exit" "$LOG"

echo "riscv64 cowstress smoke test: PASS (smp=${SMP_CPUS:-4})"
