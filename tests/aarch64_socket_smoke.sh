#!/bin/bash
# N-10: Linux ABI のソケット syscall が EL0 から使えるかの検査。
#
# **kernel/net_socket.c は前から TCP/UDP をフル実装していたのに、
# kernel/linux_syscall.c に socket(198)/connect(203) 等の入口が無く、
# aarch64 のユーザー空間からはソケットが一切使えなかった** (x86 だけが
# 独自 syscall 経路から呼んでいた)。その入口を足したので、musl の
# ソケット API 経由で実際に通信できることをここで確かめる。
#
# 経路: guest の probe -> virtio-net -> QEMU の user network -> 10.0.2.2
#       -> ホストの localhost:PORT (このスクリプトが立てる HTTP もどき)
#
#   make aarch64-socket-smoke  がまとめてやる
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs out

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
if [ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-aarch64 ]; then
    QEMU_BIN=/opt/homebrew/bin/qemu-system-aarch64
fi
if [ -z "$QEMU_BIN" ]; then
    echo "qemu-system-aarch64 not found" >&2
    exit 1
fi

PROBE=out/aarch64-socket-probe.elf
[ -f "$PROBE" ] || { echo "missing $PROBE ('make aarch64-socket-probe')" >&2; exit 1; }

TEST_DISK=out/aarch64-socket-disk.img
TEST_FSDIR=out/aarch64-socket-fs
LOG=LOGs/aarch64-socket-serial.log
XV6FS_TEST_BLOCKS=16384
# **ポートは固定にする。** カーネルは init に引数を渡せないので、probe の
# 既定 (user/aarch64_socket_probe.c) と同じ番号でなければ繋がらない。
# 誰かが先に使っていたら bind が失敗するので、下で明示的に落とす
PORT=18080
MARKER="ORTHOX-SOCKET-SMOKE-OK"

QEMU_PID=""
SRV_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null || true
    [ -n "$QEMU_PID" ] && wait "$QEMU_PID" 2>/dev/null || true
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    [ -n "$SRV_PID" ] && wait "$SRV_PID" 2>/dev/null || true
}
trap cleanup EXIT

# ---- ホスト側の待ち受け ---------------------------------------------------
# **中身に決め打ちの目印を入れる。** これが guest 側のログに出れば、
# 「繋がった」だけでなく「本当にこのサーバから受け取った」ことまで言える
python3 - "$PORT" "$MARKER" <<'PY' > LOGs/aarch64-socket-server.log 2>&1 &
import socket, sys
port = int(sys.argv[1]); marker = sys.argv[2]
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(4)
srv.settimeout(180)
try:
    while True:
        c, a = srv.accept()
        c.settimeout(10)
        try:
            c.recv(4096)
        except Exception:
            pass
        body = marker + "\n"
        c.sendall(("HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body)).encode())
        c.close()
except Exception as e:
    print("server done:", e)
PY
SRV_PID=$!

# **本当に待ち受けられたかを確かめてから先へ進む。** bind に失敗していても
# python は起動だけはするので、繋がらない相手へ向けて QEMU を回すと
# 「connect failed」で落ちて、原因がホスト側だと分からなくなる
host_listen_ok() {
    python3 -c "import socket,sys;s=socket.socket();s.settimeout(0.5);sys.exit(0 if s.connect_ex(('127.0.0.1', $PORT))==0 else 1)" 2>/dev/null
}
listen_ready=0
for _ in $(seq 1 20); do
    if host_listen_ok; then listen_ready=1; break; fi
    sleep 0.5
done
if [ "$listen_ready" -ne 1 ]; then
    echo "*** ホスト側の待ち受け (127.0.0.1:$PORT) が立たなかった" >&2
    cat LOGs/aarch64-socket-server.log >&2 || true
    exit 1
fi

make_test_disk() {
    rm -rf "$TEST_FSDIR"
    mkdir -p "$TEST_FSDIR/bin" "$TEST_FSDIR/tmp"
    cp "$PROBE" "$TEST_FSDIR/bin/socket-probe"
    printf 'ORTHOX-AARCH64-XV6FS-OK' > "$TEST_FSDIR/aarch64-m4.txt"
    rm -f "$TEST_DISK"
    XV6FS_FSSIZE=$XV6FS_TEST_BLOCKS XV6FS_NINODES=256 \
        python3 scripts/build_rootfs_xv6fs.py "$TEST_FSDIR" "$TEST_DISK" > /dev/null
}

make_test_disk
rm -f "$LOG"

# **カーネルは probe を init として起動する。**引数はカーネル側の
# AARCH64_INIT_ARGV_* で渡せないので、probe の既定 (10.0.2.2) を使い、
# ポートだけビルド時に渡す形にはせず、probe の第 2 引数の既定を使う
"$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp 1 \
    -nographic \
    -drive "file=$TEST_DISK,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 \
    -device virtio-rng-device \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -kernel out/kernel-aarch64.elf < /dev/null > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in {1..120}; do
    if grep -aq "socket-probe: done\|socket-probe: .* failed\|bootstrap user exit" "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

echo "--- AArch64 socket Serial Output (末尾) ---"
grep -a "socket-probe\|lwip. dhcp bound\|ENOSYS" "$LOG" | head -30
echo "-------------------------------------------"

fail=0
check() {   # check <説明> <固定文字列>
    if grep -aqF -- "$2" "$LOG"; then
        printf '  %-40s ok\n' "$1"
    else
        printf '  %-40s *** NG (%s)\n' "$1" "$2" >&2
        fail=1
    fi
}

check "socket(2) が fd を返した"      "socket-probe: socket ok"
check "connect(2) が成立した"          "socket-probe: connect ok after tries="
check "getsockname(2)"                 "socket-probe: getsockname ok"
check "send(2)"                        "socket-probe: send ok"
check "recv(2) でサーバの中身が届いた"  "$MARKER"
check "probe が最後まで走った"          "socket-probe: done"
# N-12: 失敗の理由が正しく伝わるか (musl の値と一致するか)
check "errno AF_INET6 = EAFNOSUPPORT"  "socket-probe: errno af_inet6=97"
check "errno 範囲外 fd = EBADF"        "socket-probe: errno badfd=9"
check "errno ソケットでない = ENOTSOCK" "socket-probe: errno notsock=88"
check "errno 未知の選択肢 = ENOPROTOOPT" "socket-probe: errno badopt=92"
check "errno の検査が全部通った"        "socket-probe: errno checks ok"

# **未実装の syscall が裏で出ていないこと。**musl は失敗を黙って迂回する
if grep -aq "ENOSYS: syscall" "$LOG"; then
    echo "*** 未実装の syscall が呼ばれた:" >&2
    grep -a "ENOSYS: syscall" "$LOG" | sed 's/^/***   /' >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "aarch64 socket smoke test: FAIL" >&2
    echo "--- ログ末尾 ---" >&2
    tail -30 "$LOG" >&2
    exit 1
fi
echo "aarch64 socket smoke test: PASS (socket/connect/getsockname/send/recv/errno、QEMU virt限定)"
