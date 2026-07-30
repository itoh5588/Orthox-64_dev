#!/bin/bash
# busybox ash を対話シェルとして起動し、シリアル (stdio) 経由でコマンドを
# 流し込んで応答を検証する。SMP_CPUS=4 のように指定すると複数 hart で走る。
# 実行前に以下でカーネルをビルドしておくこと:
#   make riscv64-kernel RISCV64_BOOTSTRAP_USER_SRC_ELF=out/busybox-riscv64-musl.elf RISCV64_BOOTSTRAP_ARG0_VALUE=sh
# (make riscv64-ash-smoke がこの手順をまとめて行う)
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
else
    echo "OpenSBI firmware not found" >&2
    exit 1
fi

SERIAL_LOG=LOGs/riscv64-ash-serial.log
rm -f "$SERIAL_LOG"

ROOTFS_IMG=out/rootfs-riscv64-xv6.img
DRIVE_ARGS=()
if [ -f "$ROOTFS_IMG" ]; then
    DRIVE_ARGS=(-drive "file=$ROOTFS_IMG,if=none,format=raw,id=vblk0" -device virtio-blk-device,drive=vblk0)
fi

(
    sleep 8
    printf 'echo interactive-ok\n'
    sleep 2
    printf 'pwd\n'
    sleep 2
    printf 'x=42; echo val=$x\n'
    sleep 2
    printf 'ls /bin\n'
    sleep 2
    printf 'cat /etc/motd\n'
    sleep 2
    printf '/bin/echo external-exec-ok\n'
    sleep 3
    printf 'echo a b c | wc\n'
    sleep 3
    printf 'echo redirect-ok > /w.txt\n'
    sleep 2
    printf 'cat /w.txt\n'
    sleep 2
    printf 'echo append-ok >> /w.txt\n'
    sleep 2
    printf 'cat /w.txt | wc -l\n'
    sleep 3
    printf 'mkdir /d 2>/dev/null; test -d /d && echo mkdir-ok\n'
    sleep 3
    printf 'cat /etc/motd > /d/copy.txt; cat /d/copy.txt\n'
    sleep 3
    printf 'rm /w.txt; cat /w.txt || echo unlink-ok\n'
    sleep 3
    # 疑似キャラクタデバイス (/dev/null)
    printf 'cat /dev/null && echo devnull-read-ok\n'
    sleep 3
    printf 'echo x > /dev/null && echo devnull-write-ok\n'
    sleep 3
    # バックグラウンドジョブ (子は stdin を /dev/null に差し替える)。
    # 変数展開を挟んでコマンド行のエコーと出力を区別する
    printf 'BG=ok; /bin/echo bg-job-$BG &\n'
    sleep 4
    printf 'echo after-bg-ok\n'
    sleep 3
    printf 'exit\n'
    sleep 3
) | "$QEMU_BIN" \
    -machine virt \
    -cpu rv64 \
    -m 512M \
    -smp "${SMP_CPUS:-1}" \
    -bios "$FW_PATH" \
    -kernel out/kernel-riscv64.elf \
    -display none \
    -serial stdio \
    -monitor none "${DRIVE_ARGS[@]}" > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    pkill -f qemu-system-riscv64 2>/dev/null || true
}
trap cleanup EXIT

for _ in {1..75}; do
    if grep -aq "bootstrap user exit" "$SERIAL_LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done

echo "--- RISC-V ash Serial Output ---"
cat "$SERIAL_LOG"
echo "--------------------------------"

grep -aq "built-in shell (ash)" "$SERIAL_LOG"
grep -aq "riscv64 supervisor timer interrupt" "$SERIAL_LOG"
grep -aq "interactive-ok" "$SERIAL_LOG"
grep -aq "^/" "$SERIAL_LOG"
grep -aq "val=42" "$SERIAL_LOG"
if [ -f "$ROOTFS_IMG" ]; then
    grep -aq "xv6fs mounted on vblk0" "$SERIAL_LOG"
    grep -aq "busybox" "$SERIAL_LOG"
    grep -aq "hello from riscv64 xv6fs rootfs" "$SERIAL_LOG"
    grep -aq "external-exec-ok" "$SERIAL_LOG"
    grep -aqE "1 +3 +6" "$SERIAL_LOG"
    # 書き込み系 (リダイレクト / 追記 / mkdir / unlink)
    grep -aq "redirect-ok" "$SERIAL_LOG"
    grep -aqE "^ *2$" "$SERIAL_LOG"          # cat /w.txt | wc -l → 2 行 (追記が効いている)
    grep -aq "mkdir-ok" "$SERIAL_LOG"
    grep -aq "unlink-ok" "$SERIAL_LOG"
    grep -aq "devnull-read-ok" "$SERIAL_LOG"
    grep -aq "devnull-write-ok" "$SERIAL_LOG"
    # `cmd &`: 子が 1 度だけ実行され (close(0) が fork に化けていないこと)、
    # かつ /dev/null の open に失敗しないこと
    grep -aq "bg-job-ok" "$SERIAL_LOG"
    [ "$(grep -ac "bg-job-ok" "$SERIAL_LOG")" = "1" ]
    ! grep -aq "can't open '/dev/null'" "$SERIAL_LOG"
    grep -aq "after-bg-ok" "$SERIAL_LOG"
fi
grep -aq "bootstrap user exit" "$SERIAL_LOG"

echo "riscv64 ash smoke test: PASS"
