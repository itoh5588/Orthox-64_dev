/*
 * P3-2: コンソール入力 (PL011 の受信割り込み) の検査。
 *
 * **確かめたいのは「寝てから起こされる」経路。**
 *
 * CONSOLE-READY を出してから read(0) に入るので、スモーク側は
 * それを見てから文字を送る。つまり probe は **必ず一度空振りして寝る。**
 * ポーリングで拾えてしまうと割り込みが死んでいても通ってしまうため、
 * この順序に意味がある:
 *
 *   1. kb_read が 0 を返す        まだ何も来ていない
 *   2. task_mark_sleeping + kb_set_waiter で寝る
 *   3. ホストが文字を送る -> PL011 の受信割り込み
 *   4. aarch64_console_rx_irq がリングへ入れて待ち手を起こす
 *   5. kb_read が読める
 *
 * 3-4 が繋がっていなければ、ここで永久に固まる (P2 で踏んだ
 * vblk_rw の完了待ちと同じ壊れ方をする)。
 */
#include <stddef.h>
#include <string.h>
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

int main(void) {
    char buf[128];
    ssize_t n;
    size_t len;

    /* **これを見てからスモークが送る。** 出す前に read へ入ると、
     * 「たまたま先に届いていた」経路になって割り込みを検証できない */
    if (write_all(1, "CONSOLE-READY\n", 14) < 0) return 10;

    n = read(0, buf, sizeof(buf) - 1);
    if (n <= 0) return 11;
    buf[n] = 0;

    /* 末尾の改行を落として 1 行に収める (判定を行単位にするため) */
    len = (size_t)n;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;

    if (write_all(1, "CONSOLE-GOT:", 12) < 0) return 12;
    if (write_all(1, buf, len) < 0) return 13;
    if (write_all(1, "\n", 1) < 0) return 14;

    /* **中身まで照合する。** 「何か読めた」だけでは、リングの読み書きが
     * ずれていても気づけない */
    if (len == 11 && memcmp(buf, "hello-stdin", 11) == 0) {
        if (write_all(1, "CONSOLE-MATCH\n", 14) < 0) return 15;
    } else {
        if (write_all(1, "CONSOLE-MISMATCH\n", 17) < 0) return 16;
        return 17;
    }

    if (write_all(1, "CONSOLE-DONE\n", 13) < 0) return 18;
    return 0;
}
