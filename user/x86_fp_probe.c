/* x86_fp_probe — SSE レジスタがタスク切り替え / fork / exec を跨いで正しく
 * 扱われるかの検査 (2026-10-11)。riscv64 の user/riscv64-bin/fpprobe.c、
 * aarch64 の user/aarch64_fp_probe.c の x86_64 版。
 *
 *   /boot/x86_fp_probe.elf [nproc] [rounds] [spin]     既定 6 12 20000000
 *
 * **これは落ちないバグを探す検査。** 保存していなければ、fork も write も
 * 成功したまま計算結果だけが静かに変わる。見るのは 3 つ:
 *
 * 1. **切り替え** (FP-WORKER): nproc 本を fork し、各自が xmm0-xmm15 と MXCSR
 *    (丸めモード) に自分だけの値を入れて、**1 つの asm の中で** 空回りしてから
 *    読み戻す。asm の中には関数呼び出しも syscall も無いので、16 本すべてが
 *    残っていなければならない。空回りの中で 65536 回ごとに rdtsc を読み、
 *    塊 1 つの時間の最大が最短の 3 倍を超えた周を「取り上げられた」と数える
 *    (0 なら切り替えを跨いでおらず、検査になっていない)。
 *    **x86 はタイマでユーザーを取り上げない** (resched を見るのは syscall の
 *    出口と idle だけ。2026-10-11 時点)。そこで塊ごとに **asm から直接
 *    sched_yield (24) を呼んで** 切り替えを起こす。カーネルが正しければ
 *    syscall を跨いでも xmm は変わらない (変わるのは rax/rcx/r11 だけ)。
 * 2. **fork の引き継ぎ** (FP-INHERIT): xmm0-xmm15 と MXCSR に値を入れて
 *    **asm から直接 fork の syscall を呼び**、子に同じ値が見えること。
 *    SysV ABI では xmm は全部 caller-saved なので、libc の fork() を挟むと
 *    途中で壊れてもよいことになり、検査の意味が無くなる。
 * 3. **exec** (FP-EXEC): 目印を入れて asm から直接 execve し、新しい像の
 *    main で目印が見えないこと。新しい像の起動路 (crt0 / musl) が一部の
 *    xmm を使うことはあるが、16 本のうち目印が残っているものを数える。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define XMM_LIST(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) \
    X(12) X(13) X(14) X(15)
#define LD(n) "movdqu " #n "*16(%[w]), %%xmm" #n "\n\t"
#define ST(n) "movdqu %%xmm" #n ", " #n "*16(%[g])\n\t"
#define CLOB(n) "xmm" #n,
#define MXCSR_DEFAULT 0x1F80U
#define MARK 0x7E7E7E7E7E7E7E7EULL

/* 16 本 x 128bit = 32 個の uint64_t */
static void fill(uint64_t* v, int id) {
    for (int i = 0; i < 32; i++) {
        v[i] = 0x5A00000000000000ULL ^ ((uint64_t)id << 40) ^ ((uint64_t)i << 32) ^
               (0x01010101ULL * (uint64_t)(i + 1));
    }
}

/* xmm0-15 と MXCSR に入れ、spin 回まわってから読み戻す。
 * *max_gap / *min_gap に、65536 回の塊 1 つにかかった rdtsc の最大と最短 */
static uint32_t sse_hold(const uint64_t* want, uint64_t* got, uint32_t mxcsr_in, uint64_t spin,
                         uint64_t* max_gap, uint64_t* min_gap) {
    uint32_t mxcsr_out = 0, mxcsr_def = MXCSR_DEFAULT;
    uint64_t prev, now, d, i, gap = 0, gmin = ~0ULL;

    spin = (spin >> 16) + 1;
    __asm__ volatile(XMM_LIST(LD)
                     "ldmxcsr %[mi]\n\t"
                     "rdtsc\n\t"
                     "shl $32, %%rdx\n\t"
                     "or %%rdx, %%rax\n\t"
                     "mov %%rax, %[p]\n\t"
                     "1: mov $65536, %[i]\n\t"
                     "3: dec %[i]\n\t"
                     "jnz 3b\n\t"
                     "rdtsc\n\t"
                     "shl $32, %%rdx\n\t"
                     "or %%rdx, %%rax\n\t"
                     "mov %%rax, %[t]\n\t"
                     "sub %[p], %%rax\n\t"
                     "cmp %[m], %%rax\n\t"
                     "jbe 4f\n\t"
                     "mov %%rax, %[m]\n\t"
                     "4: cmp %[mn], %%rax\n\t"
                     "jae 2f\n\t"
                     "mov %%rax, %[mn]\n\t"
                     "2: mov %[t], %[p]\n\t"
                     "mov $24, %%eax\n\t"          /* sched_yield */
                     "syscall\n\t"
                     "dec %[n]\n\t"
                     "jnz 1b\n\t"
                     XMM_LIST(ST)
                     "stmxcsr %[mo]\n\t"
                     "ldmxcsr %[md]\n\t"     /* 丸めモードを戻す: 後の C の計算に効かせない */
                     : [n] "+r"(spin), [p] "=&r"(prev), [t] "=&r"(now), [m] "+r"(gap),
                       [i] "=&r"(i), [mo] "=m"(mxcsr_out), "=&d"(d), [mn] "+r"(gmin)
                     : [w] "r"(want), [g] "r"(got), [mi] "m"(mxcsr_in), [md] "m"(mxcsr_def)
                     : "memory", "rax", "rcx", "r11", XMM_LIST(CLOB) "cc");
    *max_gap = gap;
    *min_gap = gmin;
    return mxcsr_out;
}

static double series(int n) {
    double s = 0.0;
    for (int i = 1; i <= n; i++) s += 1.0 / ((double)i * (double)i) + (double)(i % 7) * 1e-9;
    return s;
}

static int worker(int id, int rounds, uint64_t spin, double expect) {
    uint64_t want[32], got[32], gap, gmin, t0 = __builtin_ia32_rdtsc();
    /* RC (bits 14:13) を id ごとに変える。例外マスクは既定のまま */
    uint32_t mxcsr = MXCSR_DEFAULT | ((uint32_t)(id % 4) << 13);
    int bad = 0, preempted = 0;

    fill(want, id);
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 32; i++) got[i] = 0;
        uint32_t mo = sse_hold(want, got, mxcsr, spin, &gap, &gmin);
        if (gap > 3 * gmin) preempted++;
        for (int i = 0; i < 32; i++) {
            if (got[i] != want[i]) {
                if (bad < 4) {
                    printf("FP-CLOBBERED id=%d round=%d xmm%d.%d want=%016llx got=%016llx\n", id,
                           r, i / 2, i % 2, (unsigned long long)want[i],
                           (unsigned long long)got[i]);
                }
                bad++;
            }
        }
        if ((mo & 0xFFC0) != (mxcsr & 0xFFC0)) {     /* 例外フラグ (5:0) は計算で立ちうる */
            printf("FP-CLOBBERED id=%d round=%d mxcsr want=%04x got=%04x\n", id, r, mxcsr, mo);
            bad++;
        }
        if (series(20000) != expect) {
            printf("FP-CLOBBERED id=%d round=%d series\n", id, r);
            bad++;
        }
    }
    printf("FP-WORKER id=%d bad=%d preempted=%d/%d tsc=%llu\n", id, bad, preempted, rounds,
           (unsigned long long)(__builtin_ia32_rdtsc() - t0));
    return bad ? 1 : 0;
}

/* xmm0-15 と MXCSR に入れて、そのまま fork の syscall (57) を呼ぶ。
 * 戻ったら親子それぞれで xmm0-15/MXCSR を got に読む */
static long sse_fork(const uint64_t* want, uint64_t* got, uint32_t mxcsr_in, uint32_t* mxcsr_out) {
    long ret;
    __asm__ volatile(XMM_LIST(LD)
                     "ldmxcsr %[mi]\n\t"
                     "mov $57, %%eax\n\t"
                     "syscall\n\t"
                     XMM_LIST(ST)
                     "stmxcsr %[mo]\n\t"
                     : "=&a"(ret), [mo] "=m"(*mxcsr_out)
                     : [w] "r"(want), [g] "r"(got), [mi] "m"(mxcsr_in)
                     : "memory", "rcx", "r11", XMM_LIST(CLOB) "cc");
    return ret;
}

static int check_inherit(void) {
    uint64_t want[32], got[32];
    uint32_t mxcsr = MXCSR_DEFAULT | (3U << 13), mo = 0, def = MXCSR_DEFAULT;
    int status = 0, bad = 0;
    long pid;

    fill(want, 99);
    pid = sse_fork(want, got, mxcsr, &mo);
    if (pid < 0) return 30;
    if (pid == 0) {
        __asm__ volatile("ldmxcsr %0" : : "m"(def));
        for (int i = 0; i < 32; i++) bad += got[i] != want[i];
        if ((mo & 0xFFC0) != (mxcsr & 0xFFC0)) bad++;
        printf(bad ? "FP-INHERIT-BAD %d\n" : "FP-INHERIT-OK\n", bad);
        fflush(stdout);
        _exit(bad ? 31 : 0);
    }
    __asm__ volatile("ldmxcsr %0" : : "m"(def));
    if (waitpid((pid_t)pid, &status, 0) < 0) return 32;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 33;
}

/* exec 直後の像: 前の像が置いた目印と MXCSR が見えないこと */
static int after_exec(void) {
    uint64_t got[32];
    uint32_t mxcsr = 0;
    int leaked = 0;
    __asm__ volatile(XMM_LIST(ST) "stmxcsr %[mo]\n\t" : [mo] "=m"(mxcsr) : [g] "r"(got) : "memory");
    for (int i = 0; i < 32; i++) leaked += got[i] == MARK;
    if ((mxcsr & 0xFFC0) != (MXCSR_DEFAULT & 0xFFC0)) leaked++;
    printf(leaked ? "FP-EXEC-LEAKED %d mxcsr=%04x\n" : "FP-EXEC-OK %d mxcsr=%04x\n", leaked,
           mxcsr);
    return leaked ? 40 : 0;
}

static int check_exec(const char* self) {
    int status = 0;
    pid_t pid = fork();
    if (pid < 0) return 41;
    if (pid == 0) {
        static uint64_t mark[32];
        static char arg1[] = "--after-exec";
        char* argv[3] = {(char*)self, arg1, 0};
        char* envp[1] = {0};
        uint32_t mxcsr = MXCSR_DEFAULT | (1U << 13);
        for (int i = 0; i < 32; i++) mark[i] = MARK;
        __asm__ volatile(XMM_LIST(LD)
                         "ldmxcsr %[mi]\n\t"
                         "mov $59, %%eax\n\t"
                         "syscall\n\t"
                         :
                         : [w] "r"(mark), [mi] "m"(mxcsr), "D"(self), "S"(argv), "d"(envp)
                         : "memory", "rax", "rcx", "r11", XMM_LIST(CLOB) "cc");
        _exit(42);
    }
    if (waitpid(pid, &status, 0) < 0) return 43;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 44;
}

int main(int argc, char** argv) {
    int nproc = 6, rounds = 12, fails = 0, rc;
    uint64_t spin = 20000000;
    double expect;

    if (argc > 1 && strcmp(argv[1], "--after-exec") == 0) return after_exec();
    if (argc > 1) nproc = atoi(argv[1]);
    if (argc > 2) rounds = atoi(argv[2]);
    if (argc > 3) spin = strtoull(argv[3], 0, 0);
    if (nproc < 1) nproc = 1;

    expect = series(20000);
    printf("FP-START nproc=%d rounds=%d spin=%llu\n", nproc, rounds, (unsigned long long)spin);
    fflush(stdout);

    if ((rc = check_inherit()) != 0) fails++;
    if ((rc = check_exec(argv[0])) != 0) fails++;

    for (int k = 0; k < nproc; k++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("FP-FORK-FAILED\n");
            return 2;
        }
        if (pid == 0) {
            int r = worker(k + 1, rounds, spin, expect);
            fflush(stdout);
            _exit(r);
        }
    }
    for (int k = 0; k < nproc; k++) {
        int status = 0;
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) fails++;
    }
    if (fails) printf("FP-FAIL %d\n", fails);
    else printf("FP-ALL-OK\n");
    return fails ? 1 : 0;
}
