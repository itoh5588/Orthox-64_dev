/*
 * P3-4: fork した子のアドレス空間が本当に返っているかの検査。
 *
 * **漏れは落ちない。** 子が exit しても空間を返さないだけなので、その場では
 * 何も起きない。ash がコマンドごとに fork するようになってから
 * 「しばらく使うと ENOMEM」という形で出てくる。
 *
 * ---- ★ 回数では捕まらない (実測) ----------------------------------------
 *
 * pmm は 0x1fcd8 = 130264 ページ (約 508MB)。fork は CoW ではなく実コピー。
 *
 * **当初「1 回 95 ページ漏れるから 1400 回で尽きる」と見積もったが外れた。**
 * 修正前の状態で実測したところ:
 *
 *   1600 回まわして **全部成功** (VMLEAK-OK が出る)
 *   空きページは 130250 -> 10097   = 1 回あたり 75 ページの漏れ
 *   枯渇するのは 1736 回目あたり
 *
 * つまり **回数だけを見る検査だと、漏れたまま緑になる。**
 * 決め手はカーネルが arch_halt_forever で出す空きページ数のほう。
 * ここを回数だけで済ませないこと。
 */
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const char* buf, size_t len) {
    while (len > 0) {
        ssize_t written = write(fd, buf, len);
        if (written <= 0) return -1;
        buf += (size_t)written;
        len -= (size_t)written;
    }
    return 0;
}

static void put_dec(int fd, unsigned long v) {
    char buf[24];
    int i = (int)sizeof(buf);
    buf[--i] = 0;
    if (v == 0) buf[--i] = '0';
    while (v > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    write_all(fd, &buf[i], sizeof(buf) - 1 - (size_t)i);
}

/* **この回数で尽きるわけではない** (実測では 1736 回目あたり)。
 * 漏れの有無は空きページ数で見る。回数は「大量にまわしても壊れない」ことと、
 * 漏れがあれば残量の差が大きく開くようにするためのもの */
#define ROUNDS 1600

int main(void) {
    int status = 0;

    if (write_all(1, "VMLEAK-START\n", 13) < 0) return 10;

    for (int i = 0; i < ROUNDS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            /* **ここで落ちるのが「漏れている」形。** 何回目で尽きたかを出す —
             * 回数が分かると、1 回あたり何ページ漏れているかを逆算できる */
            write_all(1, "VMLEAK-FORK-FAILED-AT:", 22);
            put_dec(1, (unsigned long)i);
            write_all(1, "\n", 1);
            return 11;
        }
        if (pid == 0) {
            /* 子はすぐ抜ける。**空間を返すのはカーネルの仕事** */
            _exit(0);
        }
        if (waitpid(pid, &status, 0) < 0) return 12;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 13;

        /* 進んでいることが見えるように時々出す (固まったときの切り分け用) */
        if ((i % 400) == 0) {
            write_all(1, "VMLEAK-AT:", 10);
            put_dec(1, (unsigned long)i);
            write_all(1, "\n", 1);
        }
    }

    if (write_all(1, "VMLEAK-ROUNDS:", 14) < 0) return 14;
    put_dec(1, (unsigned long)ROUNDS);
    if (write_all(1, "\n", 1) < 0) return 15;
    if (write_all(1, "VMLEAK-OK\n", 10) < 0) return 16;
    return 0;
}
