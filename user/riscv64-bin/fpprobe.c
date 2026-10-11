/* fpprobe — F/D レジスタがタスク切り替えを跨いで保たれるかの検査 (段階 7)。
 *
 *   fpprobe [nproc] [rounds] [spin]     既定 6 20 2000000
 *
 * **これは落ちないバグを探す検査。** 切り替えで f0-f31/fcsr を保存して
 * いなければ、fork も write も成功したまま計算結果だけが静かに変わる。
 * aarch64 の user/aarch64_fp_probe.c と同じ考え方。
 *
 * ---- 何を見るか ------------------------------------------------------------
 *
 * nproc 本を fork し、それぞれが自分だけの値を f0-f31 と fcsr (丸めモードと
 * 例外フラグ) に入れて、**1 つの asm の中で** 空回りしてから読み戻す。
 * asm の中には関数呼び出しも syscall も無いので、ABI の caller-saved を
 * 含めて 32 本すべてが残っていなければならない。変わっていれば、
 * その間に切り替わった相手の値を見ている。
 *
 * **本当に切り替えを跨いだかも数える。** 空回りの中で 65536 回ごとに rdtime を
 * 読み (QEMU では U モードの rdtime が例外で処理されて遅いので毎回は読まない)、
 * 前回との差の最大が GAP_TICKS を超えた回は「途中で取り上げられた」と
 * 見なす (QEMU の 10 MHz で 100 ms、Arty の 50 MHz で 20 ms。割り込みの
 * 処理だけなら桁違いに短く、持ち時間の 500 ms より十分短い)。hart 数より
 * 多く fork しないと 1 本も取り上げられず、検査が素通りする。
 *
 * もう 1 つ、普通の C の倍精度の計算 (fork 前に親が出した値と比べる) と、
 * 「exec 直後の像に前の像の FP が残っていないか」(--after-exec) も見る。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define REG_LIST(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) \
    X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23)       \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
#define FLD(n) "fld f" #n ", " #n "*8(%[w])\n\t"
#define FSD(n) "fsd f" #n ", " #n "*8(%[g])\n\t"
#define CLOB(n) "f" #n,
#define GAP_TICKS 1000000ULL

static inline uint64_t rdtime(void) {
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

/* f0-f31 と fcsr に want/fcsr_in を入れ、spin 回まわってから got/戻り値へ。
 * *max_gap には空回り中の rdtime の飛びの最大 */
static uint64_t fp_hold(const uint64_t* want, uint64_t* got, uint64_t fcsr_in, uint64_t spin,
                        uint64_t* max_gap) {
    uint64_t fcsr_out, prev, now, d, i, gap = 0;

    spin = (spin >> 16) + 1;            /* 65536 回の塊の数 */
    __asm__ volatile(REG_LIST(FLD)
                     "fscsr %[fi]\n\t"
                     "rdtime %[p]\n\t"
                     "1: li %[i], 65536\n\t"
                     "3: addi %[i], %[i], -1\n\t"
                     "bnez %[i], 3b\n\t"
                     "rdtime %[t]\n\t"
                     "sub %[d], %[t], %[p]\n\t"
                     "bleu %[d], %[m], 2f\n\t"
                     "mv %[m], %[d]\n\t"
                     "2: mv %[p], %[t]\n\t"
                     "addi %[n], %[n], -1\n\t"
                     "bnez %[n], 1b\n\t"
                     REG_LIST(FSD)
                     "frcsr %[fo]\n\t"
                     "fscsr zero\n\t"     /* 丸めモードを戻す: 後の C の計算に効かせない */
                     : [n] "+r"(spin), [fo] "=&r"(fcsr_out), [p] "=&r"(prev), [t] "=&r"(now),
                       [d] "=&r"(d), [m] "+r"(gap), [i] "=&r"(i)
                     : [w] "r"(want), [g] "r"(got), [fi] "r"(fcsr_in)
                     : "memory", REG_LIST(CLOB) "cc");
    *max_gap = gap;
    return fcsr_out;
}

/* 普通の C の倍精度計算。fork 前と後で同じ値になること */
static double series(int n) {
    double s = 0.0;
    for (int i = 1; i <= n; i++) s += 1.0 / ((double)i * (double)i) + (double)(i % 7) * 1e-9;
    return s;
}

static int worker(int id, int rounds, uint64_t spin, double expect) {
    uint64_t want[32], got[32];
    /* frm (bits 7:5) は 0-4 の正規の丸めモード、fflags (4:0) は id で変える */
    uint64_t fcsr = ((uint64_t)(id % 5) << 5) | (uint64_t)((id * 7 + 3) & 0x1f);
    uint64_t gap, t0 = rdtime();
    int bad = 0, preempted = 0;

    for (int i = 0; i < 32; i++) {
        want[i] = 0x5A00000000000000ULL ^ ((uint64_t)id << 40) ^ ((uint64_t)i << 32) ^
                  (0x01010101ULL * (uint64_t)(i + 1));
    }
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < 32; i++) got[i] = 0;
        uint64_t fo = fp_hold(want, got, fcsr, spin, &gap);
        if (gap > GAP_TICKS) preempted++;
        for (int i = 0; i < 32; i++) {
            if (got[i] != want[i]) {
                if (bad < 4) {
                    printf("FP-CLOBBERED id=%d round=%d f%d want=%016llx got=%016llx\n", id, r, i,
                           (unsigned long long)want[i], (unsigned long long)got[i]);
                }
                bad++;
            }
        }
        if ((fo & 0xff) != fcsr) {
            printf("FP-CLOBBERED id=%d round=%d fcsr want=%02llx got=%02llx\n", id, r,
                   (unsigned long long)fcsr, (unsigned long long)(fo & 0xff));
            bad++;
        }
        if (series(20000) != expect) {
            printf("FP-CLOBBERED id=%d round=%d series\n", id, r);
            bad++;
        }
    }
    printf("FP-WORKER id=%d bad=%d preempted=%d/%d ticks=%llu\n", id, bad, preempted, rounds,
           (unsigned long long)(rdtime() - t0));
    return bad ? 1 : 0;
}

/* exec 直後: 前の像が f0-f31 に残した値が見えないこと */
static int after_exec(void) {
    uint64_t got[32];
    uint64_t fcsr;
    int nonzero = 0;
    __asm__ volatile(REG_LIST(FSD) "frcsr %[fo]\n\t"
                     : [fo] "=r"(fcsr)
                     : [g] "r"(got)
                     : "memory");
    for (int i = 0; i < 32; i++) {
        if (got[i] == 0x7E7E7E7E7E7E7E7EULL) nonzero++;
    }
    printf("FP-EXEC leaked=%d fcsr=%02llx\n", nonzero, (unsigned long long)(fcsr & 0xff));
    return nonzero ? 1 : 0;
}

int main(int argc, char** argv) {
    int nproc = 6, rounds = 20, fails = 0;
    uint64_t spin = 2000000;
    double expect;

    if (argc > 1 && strcmp(argv[1], "--after-exec") == 0) return after_exec();
    if (argc > 1) nproc = atoi(argv[1]);
    if (argc > 2) rounds = atoi(argv[2]);
    if (argc > 3) spin = strtoull(argv[3], 0, 0);
    if (nproc < 1) nproc = 1;

    expect = series(20000);
    printf("FP-START nproc=%d rounds=%d spin=%llu\n", nproc, rounds, (unsigned long long)spin);
    fflush(stdout);
    for (int k = 0; k < nproc; k++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("FP-FORK-FAILED\n");
            return 2;
        }
        if (pid == 0) {
            int rc = worker(k + 1, rounds, spin, expect);
            fflush(stdout);
            _exit(rc);
        }
    }
    for (int k = 0; k < nproc; k++) {
        int status = 0;
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) fails++;
    }

    /* 前の像の FP を目印で埋めてから exec する。新しい像は 0 から始まること */
    {
        pid_t pid = fork();
        if (pid == 0) {
            uint64_t mark[32];
            for (int i = 0; i < 32; i++) mark[i] = 0x7E7E7E7E7E7E7E7EULL;
            __asm__ volatile(REG_LIST(FLD) : : [w] "r"(mark) : "memory", REG_LIST(CLOB) "cc");
            execl("/bin/fpprobe", "fpprobe", "--after-exec", (char*)0);
            _exit(3);
        }
        int status = 0;
        if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            fails++;
        }
    }

    if (fails) printf("FP-FAIL %d\n", fails);
    else printf("FP-ALL-OK\n");
    return fails ? 1 : 0;
}
