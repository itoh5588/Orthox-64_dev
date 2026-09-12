#!/bin/bash
# **段が同じページを跨ぐ ELF** が読めるかの検査。
#
# kernel/elf.c は 1 ページを 1 度しか貼らず、2 つ目の段では
# arch_vm_update_page_flags を呼ぶ。その属性が段の権限の**和**でないと、
# 共有ページは「実行できない」か「書けない」のどちらかになる。
# 普通のツールチェインは段の境目を 1 ページ空けるので、この道は専用の
# リンカ台本 (scripts/user-riscv64-straddle.ld) でしか通らない。
#   make riscv64-straddle-smoke がビルドとあわせて実行する
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
    FW_PATH=/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SERIAL_LOG=LOGs/riscv64-straddle-serial.log
rm -f "$SERIAL_LOG" "$SERIAL_LOG.nocr"

"$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp 1 \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial stdio \
    -monitor none < /dev/null > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    pkill -f qemu-system-riscv64 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..40}; do
    if grep -aq "STRADDLE-OK\|STRADDLE-BAD\|STRADDLE-NOT-SHARED" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V straddle Serial Output ---"
cat "$SERIAL_LOG"
echo "-------------------------------------"

# 判定は CR を除いたコピーに当てる (tests/riscv64_musl_smoke.sh と同じ理由)
tr -d '\r' < "$SERIAL_LOG" > "$SERIAL_LOG.nocr"
CHECK_LOG="$SERIAL_LOG.nocr"

grep -aq "STRADDLE-START" "$CHECK_LOG"
# **前提が崩れていたら緑にしない。**境目が丁度ページ境界に来ると、探針は
# 何も検査していないことになる
if grep -aq "STRADDLE-NOT-SHARED" "$CHECK_LOG"; then
    echo "段が同じページに乗っていない。リンカ台本を見直すこと" >&2
    exit 1
fi
# 共有ページのコードが実行でき、共有ページのデータに書けた
grep -aq "STRADDLE-OK" "$CHECK_LOG"
! grep -aq "STRADDLE-BAD" "$CHECK_LOG"

echo "riscv64 straddle smoke test: PASS"
