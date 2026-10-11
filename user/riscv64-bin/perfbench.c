/* perfbench — Arty S7 (MicroBlaze V) の性能の改善を数字で比べるための計測。
 *
 *   perfbench [scale]      既定 1 (QEMU なら 10 くらいでよい)
 *
 * 1 行 1 項目で「PB 名前 値 単位」を出す。比べるのは同じ基準 (同じ scale) の
 * 前後だけで、絶対値には意味を持たせない。
 *
 * **SD の読み出しは /bench.dat を最初に読む時間で測る。** xv6fs のバッファ
 * キャッシュは 4 MB あり、8 MiB の rootfs の中身はほぼ全部載るので、起動後に
 * まだ誰も読んでいないファイルでないと SD まで行かない。最後に /bench.dat を
 * 書き直して fsync するので (これが書き込みの計測)、次の起動では冷えている。
 * 初回 (ファイルが無い) は読み出しを飛ばす。
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BENCH_FILE "/bench.dat"
#define FILE_BYTES (1024 * 1024)
#define BUF_BYTES (256 * 1024)       /* D キャッシュ (8 KB) より十分大きい */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* cycle / instret はカーネルの分も数える (M モードの mcounteren で U に見せている)。
 * 1 回あたりの命令数と CPI を出し、遅いのが命令の多さかメモリ待ちかを分ける */
static inline uint64_t rdcycle(void) {
    uint64_t v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}
static inline uint64_t rdinstret(void) {
    uint64_t v;
    __asm__ volatile("rdinstret %0" : "=r"(v));
    return v;
}

static void report_cpi(const char* name, uint64_t cyc, uint64_t ins, uint64_t n) {
    printf("PB %-14s %10.0f insn %8.2f CPI\n", name, (double)ins / (double)n,
           ins ? (double)cyc / (double)ins : 0.0);
    fflush(stdout);
}

static void report(const char* name, double value, const char* unit) {
    printf("PB %-14s %10.2f %s\n", name, value, unit);
    fflush(stdout);
}

/* 中身も照合する (SD のクロックを上げたときに化けていないか)。
 * 書くのは 256 KB の塊 a[i] = i * 7 のくり返し。*bad に食い違ったバイト数 */
static int read_file(const char* path, char* buf, size_t bufsz, uint64_t* bytes, uint64_t* bad) {
    int fd = open(path, O_RDONLY);
    ssize_t n;
    *bytes = 0;
    *bad = 0;
    if (fd < 0) return -1;
    while ((n = read(fd, buf, bufsz)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            uint64_t off = *bytes + (uint64_t)i;
            if (buf[i] != (char)((off % BUF_BYTES) * 7)) (*bad)++;
        }
        *bytes += (uint64_t)n;
    }
    close(fd);
    return 0;
}

int main(int argc, char** argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    char* a = malloc(BUF_BYTES);
    char* b = malloc(BUF_BYTES);
    uint64_t t0, t1, bytes, bad;
    volatile uint64_t sink = 0;

    if (scale < 1) scale = 1;
    if (!a || !b) {
        printf("PB-FAIL malloc\n");
        return 1;
    }
    printf("PB-START scale=%d\n", scale);

    /* 1. SD から冷えたファイルを読む (前回の実行が書いたもの) */
    t0 = now_us();
    if (read_file(BENCH_FILE, a, BUF_BYTES, &bytes, &bad) == 0 && bytes > 0) {
        t1 = now_us();
        report("sd_read_cold", (double)bytes / (double)(t1 - t0), "MB/s");
        printf("PB sd_read_check  %llu bytes, %llu bad\n", (unsigned long long)bytes,
               (unsigned long long)bad);
    } else {
        printf("PB sd_read_cold (no %s yet)\n", BENCH_FILE);
    }

    /* 2. 整数演算: 依存のある加算と xor の 1 周を ns で */
    {
        uint64_t n = 5000000ULL * (uint64_t)scale, x = 1;
        t0 = now_us();
        for (uint64_t i = 0; i < n; i++) {
            x = x * 6364136223846793005ULL + i;
            x ^= x >> 29;
        }
        t1 = now_us();
        sink += x;
        report("int_loop", (double)(t1 - t0) * 1000.0 / (double)n, "ns/iter");
    }

    /* 3. memset / memcpy (256 KB をくり返す) */
    {
        int reps = 20 * scale;
        uint64_t c0 = rdcycle(), i0 = rdinstret();
        for (uint64_t i = 0, x = 1; i < 1000000; i++) x = x * 3 + i, sink += x;
        report_cpi("int_cpi", rdcycle() - c0, rdinstret() - i0, 1000000);
        c0 = rdcycle();
        i0 = rdinstret();
        memcpy(b, a, BUF_BYTES);
        report_cpi("memcpy_cpi", rdcycle() - c0, rdinstret() - i0, BUF_BYTES / 8);
        t0 = now_us();
        for (int r = 0; r < reps; r++) memset(a, r, BUF_BYTES);
        t1 = now_us();
        report("memset", (double)BUF_BYTES * reps / (double)(t1 - t0), "MB/s");
        t0 = now_us();
        for (int r = 0; r < reps; r++) {
            memcpy(b, a, BUF_BYTES);
            a[r] = (char)r;
        }
        t1 = now_us();
        sink += (uint64_t)b[7];
        report("memcpy", (double)BUF_BYTES * reps / (double)(t1 - t0), "MB/s");
    }

    /* 4. syscall 1 回 (getpid は libc を通さず直接) */
    {
        int n = 20000 * scale;
        uint64_t c0, i0;
        t0 = now_us();
        c0 = rdcycle();
        i0 = rdinstret();
        for (int i = 0; i < n; i++) sink += (uint64_t)syscall(SYS_getpid);
        report_cpi("getpid_cpi", rdcycle() - c0, rdinstret() - i0, (uint64_t)n);
        t1 = now_us();
        report("getpid", (double)(t1 - t0) / (double)n, "us");
    }

    /* 5. fork + _exit + waitpid */
    {
        int n = 50 * scale;
        uint64_t c0 = rdcycle(), i0 = rdinstret();
        t0 = now_us();
        for (int i = 0; i < n; i++) {
            pid_t pid = fork();
            if (pid == 0) _exit(0);
            if (pid > 0) waitpid(pid, 0, 0);
        }
        t1 = now_us();
        report_cpi("fork_exit_cpi", rdcycle() - c0, rdinstret() - i0, (uint64_t)n);
        report("fork_exit", (double)(t1 - t0) / (double)n, "us");
    }

    /* 6. fork + exec /bin/true + waitpid */
    {
        int n = 20 * scale;
        uint64_t c0 = rdcycle(), i0 = rdinstret();
        t0 = now_us();
        for (int i = 0; i < n; i++) {
            pid_t pid = fork();
            if (pid == 0) {
                execl("/bin/true", "true", (char*)0);
                _exit(127);
            }
            if (pid > 0) waitpid(pid, 0, 0);
        }
        t1 = now_us();
        report_cpi("fork_exec_cpi", rdcycle() - c0, rdinstret() - i0, (uint64_t)n);
        report("fork_exec", (double)(t1 - t0) / (double)n, "us");
    }

    /* 7. ファイルを書いて fsync (SD への書き込み)。次の起動の sd_read_cold 用 */
    {
        int fd = open(BENCH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            for (int i = 0; i < BUF_BYTES; i++) a[i] = (char)(i * 7);
            t0 = now_us();
            for (int done = 0; done < FILE_BYTES; done += BUF_BYTES) write(fd, a, BUF_BYTES);
            fsync(fd);
            t1 = now_us();
            close(fd);
            report("sd_write_sync", (double)FILE_BYTES / (double)(t1 - t0), "MB/s");
        } else {
            printf("PB sd_write_sync (open failed)\n");
        }
    }

    printf("PB-DONE sink=%llu\n", (unsigned long long)(sink & 0xff));
    return 0;
}
