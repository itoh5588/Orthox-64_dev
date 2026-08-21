#!/usr/bin/env bash
# Raspberry Pi 4 を触る前の立ち上げ。**PC を落とすと 3 つとも消える**ので、
# 毎朝これを 1 回走らせる。
#
#   1. USB シリアルを WSL に繋ぐ (usbipd attach)
#   2. シリアルのキャプチャを立てる  ← **Pi の電源より先に**
#   3. Windows 側の TFTP サーバを立てる ← netboot はこれが無いと SD に退く
#
# **何度走らせても安全。**既に立っているものは触らない。
#
#   bash scripts/pi4/dev_up.sh
#
# 詳細は scripts/pi4/README.md の「netboot」。
set -u

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUSID="${PI4_FTDI_BUSID:-1-8}"          # usbipd list で確認できる
TTY="${PI4_TTY:-/dev/ttyUSB0}"
NETBOOT_DIR="${PI4_NETBOOT_DIR:-/mnt/c/Users/itoh5/pi4-netboot}"
LOG="$REPO/logs/pi4/serial-$(date +%Y-%m-%d).log"

# **キャプチャの pid を探す。**pgrep -f "cat $TTY" は使わない —
# その文字列をコマンドラインに含むだけの別プロセス (この判定を走らせている
# シェル自身など) に当たって、落ちているのに「立っている」と誤判定する。
# **名前がちょうど cat のものだけ**を見て、引数に tty があるか確かめる
capture_pid() {
    local p
    for p in $(pgrep -x cat 2>/dev/null); do
        if tr '\0' '\n' < "/proc/$p/cmdline" 2>/dev/null | grep -qx -- "$TTY"; then
            printf '%s' "$p"
            return 0
        fi
    done
    return 1
}

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
warn() { printf '  \033[33m--\033[0m   %s\n' "$1"; }
ng()   { printf '  \033[31mNG\033[0m   %s\n' "$1"; }

echo "=== 1. USB シリアルを WSL に繋ぐ ==="
if [ -e "$TTY" ]; then
    ok "$TTY は既にある"
else
    if ! command -v powershell.exe >/dev/null 2>&1; then
        ng "powershell.exe が無い。WSL から Windows を呼べない"
    else
        powershell.exe -NoProfile -Command "usbipd attach --wsl --busid $BUSID" 2>&1 |
            tr -d '\r' | sed 's/^/       /'
        for _ in 1 2 3 4 5; do [ -e "$TTY" ] && break; sleep 1; done
        if [ -e "$TTY" ]; then
            ok "$TTY を繋いだ"
        else
            ng "$TTY が出てこない。powershell.exe -c 'usbipd list' で busid を確認"
            echo "       いまの busid の指定: $BUSID  (PI4_FTDI_BUSID で変えられる)"
        fi
    fi
fi

echo "=== 2. シリアルのキャプチャ ==="
if [ ! -e "$TTY" ]; then
    warn "$TTY が無いので立てられない"
elif CAP=$(capture_pid); then
    ok "既に取っている (pid $CAP)"
else
    mkdir -p "$REPO/logs/pi4"
    # 115200 8N1。**raw と -echo を忘れると化ける**
    stty -F "$TTY" 115200 cs8 -cstopb -parenb -crtscts -ixon -ixoff raw -echo 2>/dev/null \
        || ng "stty に失敗した (dialout グループに入っているか)"
    nohup cat "$TTY" >> "$LOG" 2>/dev/null &
    sleep 1
    if CAP=$(capture_pid); then
        ok "$LOG に取り始めた (pid $CAP)"
    else
        ng "キャプチャが立たなかった"
    fi
fi

echo "=== 3. TFTP サーバ (Windows 側) ==="
if [ ! -d "$NETBOOT_DIR/root" ]; then
    ng "$NETBOOT_DIR/root が無い。README の「netboot」で作る"
elif powershell.exe -NoProfile -Command \
        "if (Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" \
        >/dev/null 2>&1; then
    ok "既に 69 番を掴んでいる"
else
    WIN_DIR="$(printf '%s' "$NETBOOT_DIR" | sed 's#^/mnt/\([a-z]\)#\U\1:#; s#/#\\#g')"
    # **& で放す。**Start-Process にリダイレクトを付けると powershell.exe が
    # パイプを掴んだまま戻らない。サーバ自体は立つので、**港で判定する**
    powershell.exe -NoProfile -Command \
        "Start-Process -FilePath python -ArgumentList '$WIN_DIR\\tftp_server.py','--root','$WIN_DIR\\root' -RedirectStandardOutput '$WIN_DIR\\server.log' -RedirectStandardError '$WIN_DIR\\server.err' -WindowStyle Hidden" \
        >/dev/null 2>&1 &
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        powershell.exe -NoProfile -Command \
            "if (Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" \
            >/dev/null 2>&1 && break
    done
    if powershell.exe -NoProfile -Command \
            "if (Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }" \
            >/dev/null 2>&1; then
        ok "立てた (配信ルート $NETBOOT_DIR/root)"
        ls "$NETBOOT_DIR/root" 2>/dev/null | tr '\n' ' ' | sed 's/^/       /;s/$/\n/'
    else
        ng "立たなかった。$NETBOOT_DIR/server.err を見る"
    fi
fi

echo
echo "ここまで ok なら Pi の電源を入れてよい。"
echo "カーネルを差し替えるとき:"
echo "  make aarch64-pi4-netboot AARCH64_PCIE_BRCM_INIT=1 AARCH64_INIT_PATH_VALUE=/bin/ash"
