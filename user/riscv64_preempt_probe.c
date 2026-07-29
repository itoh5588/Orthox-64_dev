/*
 * riscv64 タイマープリエンプション検証プローブ (musl 静的リンク)
 *
 * 手順:
 *   1. pipe を 2 本作って fork する
 *   2. 子は go パイプの read で待機 (親が確実に先に進めるようにする)
 *   3. 親は go を送ってから ack パイプの read で眠る
 *   4. 子は ack に 1 バイト書いた後、syscall を一切発行しない純計算ループへ
 *      入って CPU を占有し続ける (自発的に CPU を手放さない)
 *
 * 子が回り続けている以上、親が再び走れるのはタイマー割り込みによる
 * プリエンプションだけ。親が PREEMPT-OK を出せればユーザーモードの
 * プリエンプションが効いている証拠になる (協調切替のみなら親は固まる)。
 */
#include <stddef.h>
#include <stdint.h>
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
    int go[2];
    int ack[2];
    pid_t pid;
    char c = 0;

    if (pipe(go) < 0) return 10;
    if (pipe(ack) < 0) return 11;

    pid = fork();
    if (pid < 0) return 12;

    if (pid == 0) {
        /* 子: 親の合図を待ってから CPU を占有し続ける */
        volatile uint64_t spin = 0;
        if (read(go[0], &c, 1) != 1) _exit(13);
        if (write_all(ack[1], "R", 1) < 0) _exit(14);
        for (;;) {
            spin = spin + 1;
        }
    }

    if (write_all(1, "PARENT-WAIT\n", 12) < 0) return 15;
    if (write_all(go[1], "g", 1) < 0) return 16;

    /* ここで眠り、子に CPU を明け渡す */
    if (read(ack[0], &c, 1) != 1 || c != 'R') return 17;

    /* 到達できた = CPU を回し続ける子からプリエンプトして親へ戻れた */
    if (write_all(1, "PREEMPT-OK\n", 11) < 0) return 18;
    return 0;
}
