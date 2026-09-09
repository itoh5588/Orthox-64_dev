#!/bin/bash
# TLS (HTTPS) が aarch64 の EL0 から使えるかの検査。2026-09-05 新設。
#
# **これまで TLS には自動試験が 1 つも無かった。**BearSSL は x86 向けに
# 移植済みだったが、ビルド規則が Makefile に無く (user/httpsfetch.elf は
# 2026-08-08 の作り置きが置いてあるだけ)、ソースから再現できなかった。
#
# 確かめるのは 4 つ。**どれも「動いた」ではなく「正しい」を見る**:
#
#   1. 乱数源が有り、出てくる値が毎回変わること
#      -> TLS の鍵の材料。予測できると「TLS の形はしているが守られて
#         いない」ものになる。/dev/zero を対照に置いて、測定系そのものが
#         決定的であることも同時に示す
#   2. カーネルが DHCP の結果を /etc/resolv.conf に書くこと
#      -> musl の getaddrinfo が引く先。無いと名前で繋げない
#   3. **信用できない証明書を弾くこと** (この試験の中心)
#      -> ホストに自己署名の HTTPS を立て、guest から繋いで
#         x509=62 (BR_ERR_X509_NOT_TRUSTED) になることを見る。
#         ここが 62 なら、握手・暗号・証明書の解析・鎖の検証まで
#         全部が動いたうえで**正しく拒んだ**と言える。
#         **通ってしまったら試験は失敗**にする
#      -> 54 (EXPIRED) が出たら時計が狂っている印なので、別に報せる
#   4. 実在のサイトへ本当に繋がること (インターネットが無ければ飛ばす)
#
#   make aarch64-https-smoke  がまとめてやる
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs out

QEMU_BIN="$(command -v qemu-system-aarch64 2>/dev/null || true)"
[ -z "$QEMU_BIN" ] && [ -x /opt/homebrew/bin/qemu-system-aarch64 ] && QEMU_BIN=/opt/homebrew/bin/qemu-system-aarch64
[ -z "$QEMU_BIN" ] && { echo "qemu-system-aarch64 not found" >&2; exit 1; }

HF=out/aarch64-httpsfetch.elf
ASH=out/busybox-aarch64-musl.elf
KERNEL=out/kernel-aarch64.elf
[ -f "$HF" ]     || { echo "missing $HF ('make aarch64-httpsfetch')" >&2; exit 1; }
[ -f "$ASH" ]    || { echo "missing $ASH ('make aarch64-busybox-musl')" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "missing $KERNEL" >&2; exit 1; }
command -v openssl >/dev/null || { echo "openssl が要る" >&2; exit 1; }

FSDIR=out/aarch64-https-fs
DISK=out/aarch64-https-disk.img
LOG=LOGs/aarch64-https-smoke.log
CERTDIR=out/aarch64-https-cert
# **ポートは固定。**カーネルは init に引数を渡せないので ash から打つ
PORT=18443
# 自己署名の証明書の CN。guest はこの名前で繋ぎ、名前は合うが
# **発行元が信用できない**ので 62 になる (56 = 名前違いと区別できる)
TESTHOST=orthox-https-test

QEMU_PID=""
SRV_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && { kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; }
    [ -n "$SRV_PID" ]  && { kill "$SRV_PID"  2>/dev/null; wait "$SRV_PID"  2>/dev/null; }
    return 0
}
trap cleanup EXIT

# ---- ホスト側: 自己署名の HTTPS を立てる ---------------------------------
rm -rf "$CERTDIR"; mkdir -p "$CERTDIR"
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "$CERTDIR/key.pem" -out "$CERTDIR/cert.pem" \
    -subj "/CN=$TESTHOST" -addext "subjectAltName=DNS:$TESTHOST" \
    >/dev/null 2>&1 || { echo "*** 証明書を作れなかった" >&2; exit 1; }

python3 - "$PORT" "$CERTDIR" <<'PY' > LOGs/aarch64-https-server.log 2>&1 &
import http.server, ssl, sys
port = int(sys.argv[1]); cd = sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"ORTHOX-HTTPS-SMOKE-OK\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a): pass
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cd + "/cert.pem", cd + "/key.pem")
srv = http.server.HTTPServer(("0.0.0.0", port), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
PY
SRV_PID=$!

# **本当に待ち受けたかを確かめてから進む。**立っていない相手へ向けて
# QEMU を回すと「繋がらない」で落ち、原因がホスト側だと分からなくなる
ready=0
for _ in $(seq 1 20); do
    if python3 -c "import socket,sys;s=socket.socket();s.settimeout(0.5);sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)" 2>/dev/null; then
        ready=1; break
    fi
    sleep 0.5
done
[ "$ready" -eq 1 ] || { echo "*** ホストの HTTPS ($PORT) が立たなかった" >&2; cat LOGs/aarch64-https-server.log >&2; exit 1; }

# ---- インターネットが有るか (4 の可否) -----------------------------------
NET_OK=0
REAL_IP=""
if REAL_IP="$(getent ahostsv4 example.com 2>/dev/null | awk 'NR==1{print $1}')" && [ -n "$REAL_IP" ]; then
    if curl -s -o /dev/null --max-time 8 "https://example.com/" 2>/dev/null; then NET_OK=1; fi
fi

# ---- guest 側のディスク ---------------------------------------------------
rm -rf "$FSDIR"
mkdir -p "$FSDIR/bin" "$FSDIR/tmp" "$FSDIR/etc"
for a in ash echo cat dd md5sum date; do cp "$ASH" "$FSDIR/bin/$a"; done
cp "$HF" "$FSDIR/bin/httpsfetch"
# **カーネルの起動時自己診断が中身まで照合する既知ファイル** (無いと
# fs selftest が BAD を出し、TLS の失敗と紛らわしい)
printf 'ORTHOX-AARCH64-XV6FS-OK' > "$FSDIR/aarch64-m4.txt"
rm -f "$DISK"
XV6FS_FSSIZE=32768 XV6FS_NINODES=256 \
    python3 scripts/build_rootfs_xv6fs.py "$FSDIR" "$DISK" > /dev/null
rm -f "$LOG"

# ---- guest を回す ---------------------------------------------------------
(
    sleep 14
    printf 'echo ---HTTPS-SMOKE-START---\n';                                    sleep 1
    printf 'cat /etc/resolv.conf\n';                                            sleep 1
    printf 'dd if=/dev/urandom bs=32 count=1 of=/tmp/a 2>/dev/null; md5sum /tmp/a\n'; sleep 2
    printf 'dd if=/dev/urandom bs=32 count=1 of=/tmp/b 2>/dev/null; md5sum /tmp/b\n'; sleep 2
    printf 'dd if=/dev/zero    bs=32 count=1 of=/tmp/z 2>/dev/null; md5sum /tmp/z\n'; sleep 2
    printf 'echo CLOCK=$(date -u +%%s)\n';                                      sleep 2
    printf 'echo ---UNTRUSTED---; httpsfetch %s %s / 10.0.2.2; echo UNTRUSTED-RC=$?\n' "$TESTHOST" "$PORT"; sleep 25
    if [ "$NET_OK" -eq 1 ]; then
        printf 'echo ---REAL---; httpsfetch example.com; echo REAL-RC=$?\n';    sleep 40
    fi
    printf 'echo ---HTTPS-SMOKE-END---\n';                                      sleep 2
    printf 'exit\n'
) | "$QEMU_BIN" \
    -machine virt \
    -cpu cortex-a72 \
    -m 512M \
    -smp 1 \
    -display none \
    -serial stdio \
    -monitor none \
    -drive "file=$DISK,if=none,format=raw,id=vblk0" \
    -device virtio-blk-device,drive=vblk0 \
    -netdev user,id=net0 \
    -device virtio-net-device,netdev=net0 \
    -device virtio-rng-device \
    -kernel "$KERNEL" > "$LOG" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 180); do
    grep -aq -- '---HTTPS-SMOKE-END---' "$LOG" 2>/dev/null && break
    sleep 1
done
kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null; QEMU_PID=""

# **判定は CR を除いたコピーに当てる** (termios の ONLCR で行末が CRLF になる)
tr -d '\r' < "$LOG" > "$LOG.nocr"

echo "--- AArch64 HTTPS smoke (要点) ---"
grep -aE 'rng:|net: |^[0-9a-f]{32}  /tmp/|nameserver|CLOCK=|ssl=|x509=|HTTP/1|httpsfetch:|UNTRUSTED-RC|REAL-RC' "$LOG.nocr" | head -30
echo "----------------------------------"

fail=0
check() {   # check <説明> <固定文字列>
    if grep -aqF -- "$2" "$LOG.nocr"; then
        printf '  %-46s ok\n' "$1"
    else
        printf '  %-46s *** NG (%s)\n' "$1" "$2" >&2
        fail=1
    fi
}
must_not() {   # must_not <説明> <固定文字列>
    if grep -aqF -- "$2" "$LOG.nocr"; then
        printf '  %-46s *** NG (出てはいけない: %s)\n' "$1" "$2" >&2
        grep -aF -- "$2" "$LOG.nocr" | head -3 | sed 's/^/***   /' >&2
        fail=1
    else
        printf '  %-46s ok\n' "$1"
    fi
}

# **打ち切りを黙って見逃さない**
grep -aq -- '---HTTPS-SMOKE-END---' "$LOG.nocr" || {
    echo "*** 最後まで走らずに打ち切った (guest が exit に届いていない)" >&2
    exit 1
}

echo "--- 判定 ---"
check "乱数源が立った"                     "rng: virtio-rng ready"
check "カーネルが resolv.conf を書いた"    "net: wrote /etc/resolv.conf"

# 1. 乱数が毎回変わること (対照つき)
RND_A="$(grep -aE '^[0-9a-f]{32}  /tmp/a$' "$LOG.nocr" | head -1 | cut -d' ' -f1)"
RND_B="$(grep -aE '^[0-9a-f]{32}  /tmp/b$' "$LOG.nocr" | head -1 | cut -d' ' -f1)"
ZERO="$(grep -aE '^[0-9a-f]{32}  /tmp/z$' "$LOG.nocr" | head -1 | cut -d' ' -f1)"
if [ -n "$RND_A" ] && [ -n "$RND_B" ] && [ "$RND_A" != "$RND_B" ]; then
    printf '  %-46s ok  (%s / %s)\n' "urandom の中身が 2 回とも違う" "${RND_A:0:8}" "${RND_B:0:8}"
else
    printf '  %-46s *** NG (a=%s b=%s)\n' "urandom の中身が 2 回とも違う" "$RND_A" "$RND_B" >&2
    fail=1
fi
# 対照: /dev/zero は決まった値。**測定系が決定的であることの証拠**
if [ "$ZERO" = "70bc8f4b72a86921468bf8e8441dce51" ]; then
    printf '  %-46s ok\n' "対照 (/dev/zero) が既知の値"
else
    printf '  %-46s *** NG (%s)\n' "対照 (/dev/zero) が既知の値" "$ZERO" >&2
    fail=1
fi

# 2. 時計。**ビルド時刻より前なら合っていない**
CLOCK="$(grep -a '^CLOCK=' "$LOG.nocr" | head -1 | cut -d= -f2)"
NOW="$(date -u +%s)"
if [ -n "$CLOCK" ] && [ "$CLOCK" -gt $((NOW - 86400)) ] && [ "$CLOCK" -lt $((NOW + 86400)) ]; then
    printf '  %-46s ok  (ホストとの差 %s 秒)\n' "壁時計がホストと 1 日以内" "$((CLOCK - NOW))"
else
    printf '  %-46s *** NG (guest=%s host=%s)\n' "壁時計がホストと 1 日以内" "$CLOCK" "$NOW" >&2
    fail=1
fi

# 3. **信用できない証明書を弾いたか** (この試験の中心)
check "自己署名を x509=62 で拒んだ"        "x509=62"
must_not "自己署名を通してしまっていない"  "ORTHOX-HTTPS-SMOKE-OK"
must_not "証明書の期限切れが出ていない"    "x509=54"
must_not "名前の不一致が出ていない"        "x509=56"

# 4. 実在サイト (インターネットが無ければ飛ばす)
if [ "$NET_OK" -eq 1 ]; then
    check "実在サイトへ TLS で繋がった"     "httpsfetch: tls handshake ok"
    check "実在サイトが 200 を返した"       "HTTP/1.1 200 OK"
    check "本文を受け取った"                "httpsfetch: response end"
else
    printf '  %-46s -- 飛ばした (インターネットが無い)\n' "実在サイトへの接続"
fi

echo "-------------"
if [ "$fail" -ne 0 ]; then
    echo "aarch64 HTTPS smoke test: FAIL" >&2
    exit 1
fi
echo "aarch64 HTTPS smoke test: PASS (乱数 / resolv.conf / 時計 / 証明書の拒否$([ "$NET_OK" -eq 1 ] && echo " / 実在サイト")、QEMU virt限定)"
