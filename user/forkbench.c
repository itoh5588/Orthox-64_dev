/*
 * fork の速さを測る (2026-09-19)。
 *
 * fork を CoW (kernel/vm_cow.c) にした前後で、3 アーキの fork がどれだけ
 * 変わったかを見る。同じソースを x86 / aarch64 / riscv64 の musl で組み、
 * 17efd30 (aarch64 / riscv64 は全ページを写す fork、x86 は旧実装の CoW) と
 * それ以降のカーネルで同じ引数で走らせて比べる。
 *
 * 親は作業領域 ws ページを mmap で取って書き込み、実ページを持たせてから、
 * 次の 3 通りを n 回ずつ繰り返す。**作業領域は bss に置かない。**bss に
 * 置くと、触っていなくてもローダーが実ページを割り当て、全ページを写す
 * fork が毎回それを写すので、ws=0 でも 4MB 分の値段が乗ってしまう
 *
 *   exit    子はすぐ _exit、親は waitpid。fork だけの値段
 *           (シェルや make の「fork して exec」に近い)
 *   pwrite  exit の後、親が ws ページに 1 語ずつ書く。CoW では書き込み
 *           禁止にされたページを親が取り戻すフォルトの値段が乗る
 *   cwrite  子が ws ページに 1 語ずつ書いてから _exit。CoW では子が
 *           全ページを写すので、CoW にとって最も不利な使い方
 *
 * 時刻は clock_gettime(CLOCK_MONOTONIC)。カーネルの分解能は 1ms なので、
 * 1 通りあたり 1 秒以上かかるよう n を選ぶこと。
 *
 *   forkbench [n] [ws]      既定 n=100。ws を省くと 0 と 1024 (4MB) の両方
 *                           (ws は最大 1024)
 *   forkbench run CMD ARGS  CMD を 1 回 fork+exec して終わるまでの時間
 *                           ("forkbench: run ms=...")。実機に time が無いので、
 *                           ash のループや gcc の値段をこれで測る
 *   出力: "forkbench: case=exit ws=1024 n=100 ms=... us_per_fork=..."
 *   最後に "forkbench: done"
 */
#define _GNU_SOURCE   /* -std=c11 で clock_gettime / MAP_ANONYMOUS を見せる */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE      4096
#define MAX_PAGES 1024

static volatile uint8_t* g_ws;

/* printf は使わない (cowstress と同じく 3 アーキで同じに組めるように) */
static void put(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    (void)write(1, s, n);
}

static void put_num(uint64_t v) {
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    put(&b[i]);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void touch(int ws, uint8_t v) {
    for (int p = 0; p < ws; p++) g_ws[(size_t)p * PAGE] = v;
}

enum { CASE_EXIT, CASE_PWRITE, CASE_CWRITE };

static int run_case(int kind, const char* name, int n, int ws) {
    uint64_t t0 = now_ms();
    uint64_t ms;

    for (int i = 0; i < n; i++) {
        int status = 0;
        pid_t pid = fork();
        if (pid < 0) {
            put("forkbench: FAIL fork\n");
            return 1;
        }
        if (pid == 0) {
            if (kind == CASE_CWRITE) touch(ws, (uint8_t)(i + 1));
            _exit(0);
        }
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
            put("forkbench: FAIL wait\n");
            return 1;
        }
        if (kind == CASE_PWRITE) touch(ws, (uint8_t)(i + 1));
    }
    ms = now_ms() - t0;
    put("forkbench: case="); put(name);
    put(" ws="); put_num((uint64_t)ws);
    put(" n="); put_num((uint64_t)n);
    put(" ms="); put_num(ms);
    put(" us_per_fork="); put_num(ms * 1000ULL / (uint64_t)n);
    put("\n");
    return 0;
}

static int run_ws(int n, int ws) {
    void* m = MAP_FAILED;
    int rc;

    if (ws > 0) {
        m = mmap(NULL, (size_t)ws * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            put("forkbench: FAIL mmap\n");
            return 1;
        }
        g_ws = (volatile uint8_t*)m;
    }
    touch(ws, 0xa5);
    put("forkbench: start n="); put_num((uint64_t)n);
    put(" ws="); put_num((uint64_t)ws); put("\n");
    rc = run_case(CASE_EXIT, "exit", n, ws) ||
         run_case(CASE_PWRITE, "pwrite", n, ws) ||
         run_case(CASE_CWRITE, "cwrite", n, ws);
    if (m != MAP_FAILED) munmap(m, (size_t)ws * PAGE);
    return rc;
}

/* CMD を 1 回走らせて時間を出す。forkbench 自身は小さいので、ここの
 * fork 1 回の値段は CMD の中の fork に比べて無視できる */
static int run_cmd(char** cmd) {
    uint64_t t0 = now_ms();
    int status = 0;
    pid_t pid = fork();

    if (pid < 0) {
        put("forkbench: FAIL fork\n");
        return 1;
    }
    if (pid == 0) {
        execvp(cmd[0], cmd);
        put("forkbench: FAIL exec\n");
        _exit(127);
    }
    if (waitpid(pid, &status, 0) != pid) {
        put("forkbench: FAIL wait\n");
        return 1;
    }
    put("forkbench: run ms="); put_num(now_ms() - t0);
    put(" status="); put_num((uint64_t)status);
    put("\n");
    return 0;
}

int main(int argc, char** argv) {
    int n;

    if (argc > 2 && strcmp(argv[1], "run") == 0) return run_cmd(&argv[2]);
    n = argc > 1 ? atoi(argv[1]) : 100;

    if (n < 1) n = 1;
    if (argc > 2) {
        int ws = atoi(argv[2]);
        if (ws < 0) ws = 0;
        if (ws > MAX_PAGES) ws = MAX_PAGES;
        if (run_ws(n, ws)) return 1;
    } else {
        /* aarch64 / riscv64 では init として引数なしで走るので、
         * 作業領域なし (プログラム自身のページだけ) と 4MB の両方を測る */
        if (run_ws(n, 0)) return 1;
        if (run_ws(n, MAX_PAGES)) return 1;
    }
    put("forkbench: done\n");
    return 0;
}
