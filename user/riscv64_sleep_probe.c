/*
 * riscv64 タイマー起床経路の検証プローブ (musl 静的リンク)
 *
 * nanosleep で本当に「寝て」「起きられる」かを見る。スピン待ちで代用している
 * 間は測定できないので、カーネル側が task_mark_io_wait_until + タイマー走査で
 * 起床する実装になっていることが前提。
 *
 * 手順:
 *   1. clock_gettime(MONOTONIC) で T0 を取る
 *   2. nanosleep(200ms) を 5 回まわす
 *   3. T1 を取り、経過が 1000ms 以上 2200ms 以下なら合格
 *
 * 上限が効く: 起床させた CPU へ resched を要求しないと、起こされたタスクは
 * 走行中タスクのタイムスライスが尽きるまで待たされる (1 回あたり最大
 * TASK_TIMESLICE_TICKS 分 = riscv64 なら約 500ms 上乗せ)。実測でその状態は
 * 約 3500ms になるので、2200ms を超えたら起床レイテンシの退行とみなす。
 *
 * 経過が短すぎる = 寝ていない、戻ってこない = 起床経路が死んでいる。
 * printf は long double 経路 (__divtf3) を踏むので使わず、write と自前の
 * 10 進変換だけで報告する。
 */
#include <stddef.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
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

static int put_u64(int fd, uint64_t v) {
    char buf[24];
    int i = (int)sizeof(buf);
    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v > 0);
    return write_all(fd, &buf[i], sizeof(buf) - 1U - (size_t)i);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

int main(void) {
    struct timespec req;
    uint64_t t0;
    uint64_t t1;
    uint64_t delta;
    int i;

    if (write_all(1, "SLEEP-PROBE-START\n", 18) < 0) return 10;

    t0 = now_ms();
    for (i = 0; i < 5; i++) {
        req.tv_sec = 0;
        req.tv_nsec = 200L * 1000000L;
        if (nanosleep(&req, &req) != 0) return 11;
        if (write_all(1, "SLEEP-TICK\n", 11) < 0) return 12;
    }
    t1 = now_ms();
    delta = t1 - t0;

    if (write_all(1, "SLEEP-ELAPSED-MS ", 17) < 0) return 13;
    if (put_u64(1, delta) < 0) return 14;
    if (write_all(1, "\n", 1) < 0) return 15;

    if (delta < 1000ULL || delta > 2200ULL) {
        if (write_all(1, "SLEEP-PROBE-BAD\n", 16) < 0) return 16;
        return 1;
    }
    /* busybox の `sleep 1` と同じ形: 子が眠り、親は wait でブロックする。
     * 親子とも寝ている間は idle だけが走るので、起床経路が idle 経由でも
     * 効いていることの確認になる (直接 sleep だけでは通ってしまう) */
    {
        pid_t pid = fork();
        int status = 0;
        if (pid < 0) return 18;
        if (pid == 0) {
            req.tv_sec = 0;
            req.tv_nsec = 300L * 1000000L;
            if (nanosleep(&req, &req) != 0) _exit(3);
            _exit(0);
        }
        if (waitpid(pid, &status, 0) != pid) return 19;
        if (status != 0) return 20;
        if (write_all(1, "SLEEP-CHILD-OK\n", 15) < 0) return 21;
    }

    if (write_all(1, "SLEEP-PROBE-OK\n", 15) < 0) return 17;
    return 0;
}
