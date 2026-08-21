#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Raspberry Pi 4 を netboot するための、読み出し専用の TFTP サーバ。

**SD カードの抜き差しをやめるために作った。** Pi 4 の EEPROM ブートローダは
LAN から起動一式 (start4.elf / fixup4.dat / config.txt / DTB / kernel8.img)
を TFTP で取ってくる。**カーネルが走る前にブートローダがやる**ので、
Orthox-64 側に NIC ドライバは要らない。

## なぜ Windows 側で走らせるのか

WSL2 は NAT の中に居て LAN (192.168.11.0/24) に居ない。`netsh portproxy` は
**TCP のみで UDP を転送できない**ので、WSL 側に立てた TFTP は Pi から見えない。
WSL を mirrored networking に変える手もあるが、Docker のブリッジと衝突する。

**このスクリプトは Windows の python.exe で走らせる。**標準ライブラリだけで
書いてあるので、入れる物は何も無い。

    python scripts\pi4\tftp_server.py --root C:\pi4-netboot\root

## 書き込みは実装しない

WRQ は握り潰して ERROR を返す。**こちらから Pi へ配るだけ**で用が足りるし、
書ける口を開けておく理由が無い。

## 参照

RFC 1350 (TFTP)、RFC 2347 (option extension)、RFC 2348 (blksize)、
RFC 2349 (timeout / tsize)
"""

import argparse
import os
import socket
import struct
import sys
import threading
import time

# ---- opcode ---------------------------------------------------------------
OP_RRQ = 1
OP_WRQ = 2
OP_DATA = 3
OP_ACK = 4
OP_ERROR = 5
OP_OACK = 6

# ---- ERROR の code (RFC 1350 の 4.1) --------------------------------------
ERR_NOT_DEFINED = 0
ERR_FILE_NOT_FOUND = 1
ERR_ACCESS_VIOLATION = 2
ERR_ILLEGAL_OP = 4

DEFAULT_BLKSIZE = 512
MAX_BLKSIZE = 65464          # RFC 2348 の上限
DEFAULT_TIMEOUT = 1.0        # 1 ブロックあたりの待ち (秒)
MAX_RETRIES = 6              # 再送の上限。超えたら諦める

_print_lock = threading.Lock()


def log(msg):
    """複数スレッドから出しても行が混ざらないようにする。"""
    with _print_lock:
        sys.stdout.write("[tftp] %s\n" % msg)
        sys.stdout.flush()


def parse_request(payload):
    """RRQ / WRQ の中身を (filename, mode, options) に割る。

    書式は  opcode(2) filename\\0 mode\\0 [key\\0 value\\0]...
    """
    parts = payload.split(b"\x00")
    # 末尾が \0 で終わるので最後の空要素を落とす
    if parts and parts[-1] == b"":
        parts.pop()
    if len(parts) < 2:
        return None, None, None
    filename = parts[0].decode("latin-1")
    mode = parts[1].decode("latin-1").lower()
    options = {}
    rest = parts[2:]
    for i in range(0, len(rest) - 1, 2):
        options[rest[i].decode("latin-1").lower()] = rest[i + 1].decode("latin-1")
    return filename, mode, options


def resolve(root, filename):
    """要求されたファイル名を実ファイルへ落とす。

    2 段構えにしてある。

    1. そのままの相対パス
    2. **先頭のディレクトリを 1 段落として**もう一度

    2 が要るのは、Pi 4 のブートローダが既定で
    ``<シリアル番号>/start4.elf`` のようにシリアル番号のディレクトリを
    前置きするため。**こちらでシリアル番号を知らなくても配れる**ようにする
    (EEPROM の TFTP_PREFIX を 0 にする手もあるが、設定を 1 つ減らせる)。

    戻り値は (実パス または None, 実際に効いた要求名)。
    """
    name = filename.replace("\\", "/").lstrip("/")
    candidates = [name]
    if "/" in name:
        candidates.append(name.split("/", 1)[1])

    root_real = os.path.realpath(root)
    for cand in candidates:
        if not cand:
            continue
        path = os.path.realpath(os.path.join(root_real, cand))
        # **root の外に出る要求は弾く。** ".." でも symlink でも同じ判定になる
        if path != root_real and not path.startswith(root_real + os.sep):
            continue
        if os.path.isfile(path):
            return path, cand
    return None, None


def send_error(sock, addr, code, message):
    packet = struct.pack("!HH", OP_ERROR, code) + message.encode("latin-1") + b"\x00"
    try:
        sock.sendto(packet, addr)
    except OSError:
        pass


def negotiate(options, filesize):
    """こちらが受けるオプションだけ選んで (採用値, OACK に載せるもの) を返す。

    **知らないオプションは黙って落とす。** RFC 2347 のとおり、OACK に
    含めなければ「使わない」と伝わる。
    """
    blksize = DEFAULT_BLKSIZE
    timeout = DEFAULT_TIMEOUT
    accepted = []

    if "blksize" in options:
        try:
            want = int(options["blksize"])
        except ValueError:
            want = -1
        if 8 <= want <= MAX_BLKSIZE:
            blksize = want
            accepted.append(("blksize", str(blksize)))

    if "timeout" in options:
        try:
            want = int(options["timeout"])
        except ValueError:
            want = -1
        if 1 <= want <= 255:
            timeout = float(want)
            accepted.append(("timeout", str(want)))

    # RRQ の tsize は「大きさを教えろ」の意味なので、要求値は 0 で来る。
    # **0 バイトのファイルでは載せない。** RFC 2349 上は 0 も正しい答えだが、
    # curl は OACK の tsize=0 を無効と見て降りる。オプションは
    # 「返さなければ使わない」で通るので、載せないほうが相手を選ばない
    if "tsize" in options and filesize > 0:
        accepted.append(("tsize", str(filesize)))

    return blksize, timeout, accepted


def serve_file(client_addr, path, req_name, options, bind_ip):
    """1 件の RRQ を、専用のソケット (TID) で最後まで面倒を見る。"""
    filesize = os.path.getsize(path)
    blksize, timeout, accepted = negotiate(options, filesize)

    # **転送ごとに新しいポートを開く。** TFTP はこれを TID と呼び、
    # 以降のやりとりは 69 番ではなくこちらで行う
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_ip, 0))
    sock.settimeout(timeout)

    opt_desc = " ".join("%s=%s" % kv for kv in accepted) or "オプション無し"
    log("送出 %s -> %s:%d  %d バイト  %s"
        % (req_name, client_addr[0], client_addr[1], filesize, opt_desc))

    started = time.time()
    try:
        with open(path, "rb") as f:
            block = 0

            # ---- オプションを受けたなら、DATA の前に OACK を返す ----------
            if accepted:
                packet = struct.pack("!H", OP_OACK)
                for key, value in accepted:
                    packet += key.encode("latin-1") + b"\x00" + value.encode("latin-1") + b"\x00"
                if not _send_and_wait_ack(sock, client_addr, packet, 0, timeout):
                    log("中断 %s  OACK に ACK が返らない" % req_name)
                    return

            # ---- 本体 ------------------------------------------------------
            while True:
                chunk = f.read(blksize)
                block = (block + 1) & 0xFFFF   # 65535 の次は 0 に巻く
                packet = struct.pack("!HH", OP_DATA, block) + chunk
                # **最初の 1 個だけ再送を短くする。**Pi のブートローダは
                # 「その名前があるか」を確かめるためだけに RRQ を投げ、
                # **OACK の tsize を見たら中身を受け取らずに降りる**
                # (公式文書の TFTP_PREFIX: prefixed ディレクトリに start4.elf が
                #  無ければ prefix を消す、の判定がこれ)。降りた相手に
                # 7 秒も投げ続ける意味が無い。**2 個目から先は通常の予算に戻す**
                budget = 2 if block == 1 else MAX_RETRIES
                if not _send_and_wait_ack(sock, client_addr, packet, block, timeout, budget):
                    if block == 1:
                        log("打ち切り %s  相手が降りた (存在確認の探針とみられる)"
                            % req_name)
                    else:
                        log("中断 %s  block %d の ACK が返らない" % (req_name, block))
                    return
                # **最後のブロックは blksize 未満。** ちょうど割り切れる場合は
                # 長さ 0 のブロックを 1 つ送って終わる (RFC 1350)
                if len(chunk) < blksize:
                    break

        elapsed = time.time() - started
        rate = (filesize / elapsed / 1024.0) if elapsed > 0 else 0.0
        log("完了 %s  %d バイト  %.2f 秒  %.0f KB/s" % (req_name, filesize, elapsed, rate))
    except OSError as exc:
        log("失敗 %s  %s" % (req_name, exc))
        send_error(sock, client_addr, ERR_NOT_DEFINED, str(exc))
    finally:
        sock.close()


def _send_and_wait_ack(sock, addr, packet, expect_block, timeout, retries=MAX_RETRIES):
    """1 つ送って、その ACK が返るまで面倒を見る。返れば True。

    **同じ ACK が二重に来ることがある。** 期待より古い block の ACK は
    「相手がまだ前の所に居る」印なので、再送はせず待ち続ける
    (sorcerer's apprentice syndrome を起こさないため)。
    """
    for attempt in range(retries + 1):
        try:
            sock.sendto(packet, addr)
        except OSError:
            return False

        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break                      # 時間切れ。外側の for で再送する
            sock.settimeout(remaining)
            try:
                data, peer = sock.recvfrom(1024)
            except socket.timeout:
                break
            except OSError:
                return False
            if peer != addr:
                continue                   # 別の相手からの迷子は捨てる
            if len(data) < 4:
                continue
            opcode, block = struct.unpack("!HH", data[:4])
            if opcode == OP_ERROR:
                return False               # 相手が降りた
            if opcode != OP_ACK:
                continue
            if block == expect_block:
                return True
            # 古い ACK。再送せずに待ち直す
    return False


def handle_request(payload, client_addr, root, bind_ip):
    opcode = struct.unpack("!H", payload[:2])[0]
    body = payload[2:]

    if opcode == OP_WRQ:
        filename, _, _ = parse_request(body)
        log("拒否 書き込み要求 %s (%s:%d)" % (filename, client_addr[0], client_addr[1]))
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind((bind_ip, 0))
        send_error(sock, client_addr, ERR_ACCESS_VIOLATION, "read-only server")
        sock.close()
        return

    if opcode != OP_RRQ:
        return

    filename, mode, options = parse_request(body)
    if filename is None:
        return

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind_ip, 0))

    if mode not in ("octet", "netascii"):
        log("拒否 未対応のモード %s (%s)" % (mode, filename))
        send_error(sock, client_addr, ERR_ILLEGAL_OP, "unsupported mode")
        sock.close()
        return

    path, req_name = resolve(root, filename)
    if path is None:
        # **無いものは無いと返す。** 黙っていると相手が再送を繰り返して
        # 起動が延びる。ブートローダは cmdline.txt などを投機的に要求する
        log("無し %s (%s:%d)" % (filename, client_addr[0], client_addr[1]))
        send_error(sock, client_addr, ERR_FILE_NOT_FOUND, "file not found")
        sock.close()
        return

    sock.close()
    serve_file(client_addr, path, req_name, options, bind_ip)


def main():
    parser = argparse.ArgumentParser(
        description="Pi 4 の netboot 用 読み出し専用 TFTP サーバ")
    parser.add_argument("--root", required=True,
                        help="配信するディレクトリ (この外は絶対に読ませない)")
    parser.add_argument("--bind", default="0.0.0.0",
                        help="待ち受けるアドレス (既定: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=69,
                        help="待ち受けるポート (既定: 69)")
    args = parser.parse_args()

    root = os.path.realpath(args.root)
    if not os.path.isdir(root):
        sys.stderr.write("エラー: --root が見つからない: %s\n" % root)
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((args.bind, args.port))
    except OSError as exc:
        sys.stderr.write("エラー: %s:%d を掴めない: %s\n" % (args.bind, args.port, exc))
        return 1

    log("配信ルート %s" % root)
    log("待ち受け %s:%d" % (args.bind, args.port))
    entries = sorted(os.listdir(root))
    log("直下のもの: %s" % (" ".join(entries) if entries else "(空)"))
    log("止めるときは Ctrl+C")

    try:
        while True:
            try:
                payload, client_addr = sock.recvfrom(2048)
            except OSError as exc:
                log("受信に失敗: %s" % exc)
                continue
            if len(payload) < 4:
                continue
            thread = threading.Thread(
                target=handle_request,
                args=(payload, client_addr, root, args.bind),
                daemon=True)
            thread.start()
    except KeyboardInterrupt:
        log("止めた")
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
