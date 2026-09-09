/*
 * N-10: Linux ABI のソケット syscall が EL0 から使えるかの検査。
 *
 * **kernel/net_socket.c は前から TCP/UDP をフル実装していたのに、
 * kernel/linux_syscall.c に socket(198)/connect(203) 等が無く、
 * aarch64 のユーザー空間からは一切使えなかった** (x86 だけが独自 syscall
 * 経路 kernel/sys_net.c から呼んでいた)。その入口を足したので、実際に
 * musl のソケット API 経由で通信できることをここで確かめる。
 *
 * 接続先は引数で受ける。QEMU の user network では 10.0.2.2 がホストの
 * localhost に繋がるので、ホスト側で待ち受けたサーバへ届く。
 *
 * **判定しやすいよう、節目ごとに 1 行ずつ出す。** どこまで進んだかが
 * そのまま切り分けになる (socket は通ったが connect で落ちた、など)。
 */
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

static void say(const char* s) {
    (void)write(1, s, strlen(s));
}

static void say_num(const char* label, long v) {
    char buf[32];
    int pos = 0;
    int neg = 0;
    say(label);
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[pos++] = '0';
    while (v > 0 && pos < 20) { buf[pos++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) buf[pos++] = '-';
    {
        char out[32];
        int i;
        for (i = 0; i < pos; i++) out[i] = buf[pos - 1 - i];
        out[pos] = '\n';
        (void)write(1, out, (size_t)pos + 1);
    }
}

/* "10.0.2.2" をネットワークバイト順の 32bit へ */
static int parse_ipv4(const char* s, unsigned int* out) {
    unsigned int parts[4];
    int count = 0;
    const char* p = s;
    if (!s || !out) return -1;
    while (*p && count < 4) {
        unsigned int v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10U + (unsigned int)(*p - '0'); p++; digits++; }
        if (!digits || v > 255U) return -1;
        parts[count++] = v;
        if (*p == '.') p++;
        else if (*p != '\0') return -1;
    }
    if (count != 4 || *p != '\0') return -1;
    /* ネットワークバイト順 (ビッグエンディアン) で組む */
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 0;
}

int main(int argc, char** argv) {
    const char* host = (argc > 1) ? argv[1] : "10.0.2.2";
    /* 既定は tests/aarch64_socket_smoke.sh が立てる待ち受けと揃えてある。
     * **カーネルは init に引数を渡せない**ので、probe を init として
     * 起動する smoke ではこの既定がそのまま使われる */
    int port = (argc > 2) ? atoi(argv[2]) : 18080;
    unsigned int ip = 0;
    struct sockaddr_in sa;
    char req[128];
    char buf[512];
    int fd;
    ssize_t n;
    int total = 0;

    if (parse_ipv4(host, &ip) != 0) {
        say("socket-probe: bad address\n");
        return 1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        say_num("socket-probe: socket failed errno=", errno);
        return 1;
    }
    say_num("socket-probe: socket ok fd=", fd);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    /* htons/htonl を使わず自分で並べる (エンディアン依存を持ち込まない) */
    sa.sin_port = (unsigned short)(((port & 0xff) << 8) | ((port >> 8) & 0xff));
    sa.sin_addr.s_addr = (unsigned int)(((ip & 0xffU) << 24) | (((ip >> 8) & 0xffU) << 16) |
                                        (((ip >> 16) & 0xffU) << 8) | ((ip >> 24) & 0xffU));

    /* **DHCP が終わるのを待つ。** カーネルは init をすぐ起動するので、
     * probe の方が IP の取得より先に走る。IP が無いうちは lwIP が
     * 「経路が無い」を返す (kernel/net_socket.c の ORTH_ERR_EHOSTUNREACH
     * = 118。実測でここに当たった)。**その間だけ黙って待ち、それ以外の
     * 失敗は即座に報告する** —— 何でもリトライすると本当の不具合を
     * 待ち時間で覆い隠してしまう */
    {
        int tries;
        int connected = 0;
        for (tries = 0; tries < 30; tries++) {
            if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
                connected = 1;
                break;
            }
            /* **N-12 より前は 118 が返っていた** (kernel/net_socket.c の
             * 独自定義がずれていた)。いまは musl と同じ 113。古い
             * カーネルでも待てるよう両方を見る */
            if (errno != 118 && errno != EHOSTUNREACH &&
                errno != ENETUNREACH && errno != EINPROGRESS && errno != EALREADY) {
                say_num("socket-probe: connect failed errno=", errno);
                close(fd);
                return 1;
            }
            /* **fd を作り直す。** 失敗した TCP ソケットは同じ fd で
             * connect をやり直せない (lwIP の pcb が閉じている) */
            close(fd);
            sleep(1);
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                say_num("socket-probe: socket failed errno=", errno);
                return 1;
            }
        }
        if (!connected) {
            say("socket-probe: connect failed (no route after 30s - DHCP did not finish?)\n");
            close(fd);
            return 1;
        }
        say_num("socket-probe: connect ok after tries=", (long)tries);
    }

    /* getsockname が自分の割り当てを返せるか (N-10 で入口を足した口の 1 つ) */
    {
        struct sockaddr_in local;
        socklen_t len = sizeof(local);
        memset(&local, 0, sizeof(local));
        if (getsockname(fd, (struct sockaddr*)&local, &len) == 0) {
            say("socket-probe: getsockname ok\n");
        } else {
            say_num("socket-probe: getsockname failed errno=", errno);
        }
    }

    strcpy(req, "GET / HTTP/1.0\r\n\r\n");
    n = send(fd, req, strlen(req), 0);
    if (n < 0) {
        say_num("socket-probe: send failed errno=", errno);
        close(fd);
        return 1;
    }
    say_num("socket-probe: send ok bytes=", (long)n);

    for (;;) {
        n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        total += (int)n;
        if (total <= (int)sizeof(buf)) {
            /* 最初の塊だけ中身を見せる。**印字できない文字は . にする** */
            int i;
            buf[n] = '\0';
            for (i = 0; i < (int)n; i++) {
                if (buf[i] != '\n' && (buf[i] < 0x20 || buf[i] > 0x7e)) buf[i] = '.';
            }
            say("socket-probe: first chunk >>>\n");
            say(buf);
            say("\n<<<\n");
        }
    }
    say_num("socket-probe: recv total=", (long)total);

    close(fd);

    /* ---- errno の検査 (N-12, 2026-09-04) ---------------------------------
     *
     * **失敗の理由が正しく伝わるかを見る。** kernel/net_socket.c は失敗時に
     * -1 を返す箇所が多く、musl からは一律 EPERM (1) に見えていた。独自
     * 定義の値も 4 つ Linux とずれていた (EHOSTUNREACH が 118 など)。
     * 期待値は musl の bits/errno.h のもの。**成功パスだけ見ていると
     * ここは永久に気付けない**ので、わざと失敗させて確かめる */
    {
        int e;
        int bad = 0;

        /* IPv6 は持っていない -> EAFNOSUPPORT (97) */
        errno = 0;
        if (socket(10 /* AF_INET6 */, SOCK_STREAM, 0) >= 0) {
            say("socket-probe: errno AF_INET6 unexpectedly succeeded\n");
            bad = 1;
        } else {
            e = errno;
            say_num("socket-probe: errno af_inet6=", e);
            if (e != EAFNOSUPPORT) bad = 1;
        }

        /* 範囲外の fd -> EBADF (9) */
        errno = 0;
        if (connect(999, (struct sockaddr*)&sa, sizeof(sa)) >= 0) {
            say("socket-probe: errno badfd unexpectedly succeeded\n");
            bad = 1;
        } else {
            e = errno;
            say_num("socket-probe: errno badfd=", e);
            if (e != EBADF) bad = 1;
        }

        /* ソケットでない fd (stdin) -> ENOTSOCK (88) */
        {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            errno = 0;
            if (getsockname(0, (struct sockaddr*)&local, &len) >= 0) {
                say("socket-probe: errno notsock unexpectedly succeeded\n");
                bad = 1;
            } else {
                e = errno;
                say_num("socket-probe: errno notsock=", e);
                if (e != ENOTSOCK) bad = 1;
            }
        }

        /* 知らない選択肢 -> ENOPROTOOPT (92) */
        {
            int fd2 = socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            if (fd2 >= 0) {
                errno = 0;
                if (setsockopt(fd2, SOL_SOCKET, 12345, &one, sizeof(one)) >= 0) {
                    say("socket-probe: errno optname unexpectedly succeeded\n");
                    bad = 1;
                } else {
                    e = errno;
                    say_num("socket-probe: errno badopt=", e);
                    if (e != ENOPROTOOPT) bad = 1;
                }
                close(fd2);
            }
        }

        say(bad ? "socket-probe: errno checks NG\n" : "socket-probe: errno checks ok\n");
        if (bad) {
            say("socket-probe: done\n");
            return 1;
        }
    }

    say("socket-probe: done\n");
    return (total > 0) ? 0 : 1;
}
