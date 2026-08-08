/* pipe / FIFO の端ごとの本数 (pipe_t の readers / writers) が正しく数えられて
 * いるかを見る。
 *
 * 元は ref_count ひとつで EOF (書き手全 close) と EPIPE (読み手全 close) を
 * 判定していた。ref_count は両端の合計なので、同じ端が 2 本開いていると
 * 「片方の端が全部閉じた」を検出できない。
 *
 *   dupeof     匿名 pipe。読み端を dup してから書き端を閉じると EOF が来るか
 *   sigpipe    匿名 pipe。読み端を全部閉じた先への write が EPIPE で返るか
 *   fifoeof    FIFO を 2 本の読み手で開き、書き手を閉じたら EOF が来るか
 *   fifoepipe  FIFO を 2 本の書き手で開き、読み手を閉じたら EPIPE が返るか
 *
 * fifoeof は修正前だと read が返らない (ハングする)。逆確認のときは
 * fifoeof= の行が出ないまま止まるのが期待される壊れ方。
 *
 * SIGPIPE は上げない設計なので、EPIPE の 2 つは戻り値で判定できる。
 * SIGPIPE を上げるようにしたら、ここは signal(SIGPIPE, SIG_IGN) が要る。 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FIFO_PATH "/tmp/pipeend.fifo"

static int fail(const char* what) {
    printf("PIPEEND: FAIL %s (errno=%d)\n", what, errno);
    return 1;
}

/* 匿名 pipe: 読み端を dup してから書き端を閉じても EOF が来ること。
 * 読み端が 2 本あるので、合計本数で見ていると EOF 条件に届かない。 */
static int test_dupeof(void) {
    int fds[2];
    int dup_read;
    char buf[8];
    ssize_t n;

    if (pipe(fds) < 0) return fail("dupeof pipe");
    dup_read = dup(fds[0]);
    if (dup_read < 0) return fail("dupeof dup");

    if (write(fds[1], "abcd", 4) != 4) return fail("dupeof write");
    close(fds[1]);                       /* 書き手はこれで全滅 */

    n = read(fds[0], buf, sizeof(buf));
    if (n != 4) return fail("dupeof read data");
    n = read(fds[0], buf, sizeof(buf));  /* 修正前はここで返らない可能性 */
    if (n != 0) return fail("dupeof read eof");

    close(dup_read);
    close(fds[0]);
    printf("PIPEEND: dupeof=OK\n");
    return 0;
}

/* 匿名 pipe: 読み手が全部消えた先への write が EPIPE で返ること。
 * 修正前は判定そのものが無く、バッファに入るぶんだけ黙って成功していた。 */
static int test_sigpipe(void) {
    int fds[2];
    ssize_t n;

    if (pipe(fds) < 0) return fail("sigpipe pipe");
    close(fds[0]);

    errno = 0;
    n = write(fds[1], "abcd", 4);
    if (n >= 0) {
        printf("PIPEEND: FAIL sigpipe write returned %d\n", (int)n);
        close(fds[1]);
        return 1;
    }
    if (errno != EPIPE) return fail("sigpipe errno");

    close(fds[1]);
    printf("PIPEEND: sigpipe=OK\n");
    return 0;
}

/* FIFO は open のたびに参照が増えるので、同じ端を 2 回開くと合計本数が
 * 3 になる。書き手を閉じても合計は 2 のままで、旧条件 (ref_count < 2) は
 * 成立しない = 読み手が EOF を受け取れない。 */
static int test_fifo(void) {
    int r1, r2, w1, w2;
    char buf[8];
    ssize_t n;

    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) < 0) return fail("fifo mkfifo");

    /* 読み手 2 + 書き手 1。O_NONBLOCK は open のランデブー待ちを避けるため */
    r1 = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (r1 < 0) return fail("fifo open r1");
    r2 = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (r2 < 0) return fail("fifo open r2");
    w1 = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (w1 < 0) return fail("fifo open w1");

    if (write(w1, "z", 1) != 1) return fail("fifo write");
    close(w1);                           /* 書き手はこれで全滅 */

    n = read(r1, buf, sizeof(buf));
    if (n != 1 || buf[0] != 'z') return fail("fifo read data");
    n = read(r1, buf, sizeof(buf));      /* 修正前はここで返らない */
    if (n != 0) return fail("fifo read eof");
    close(r1);
    close(r2);
    printf("PIPEEND: fifoeof=OK\n");

    /* 読み手 1 + 書き手 2。読み手を閉じたら書き手が EPIPE を得ること */
    r1 = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (r1 < 0) return fail("fifo open r1b");
    w1 = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (w1 < 0) return fail("fifo open w1b");
    w2 = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
    if (w2 < 0) return fail("fifo open w2b");

    close(r1);                           /* 読み手はこれで全滅 */
    errno = 0;
    n = write(w1, "z", 1);
    if (n >= 0) {
        printf("PIPEEND: FAIL fifoepipe write returned %d\n", (int)n);
        return 1;
    }
    if (errno != EPIPE) return fail("fifoepipe errno");

    close(w1);
    close(w2);
    unlink(FIFO_PATH);
    printf("PIPEEND: fifoepipe=OK\n");
    return 0;
}

int main(void) {
    printf("PIPEEND: start\n");
    if (test_dupeof() != 0) return 1;
    if (test_sigpipe() != 0) return 1;
    if (test_fifo() != 0) return 1;
    printf("PIPEEND: PASS\n");
    return 0;
}
