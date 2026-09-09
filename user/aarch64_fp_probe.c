/*
 * P3-3: FP/SIMD レジスタがタスク切り替えを跨いで保たれるかの検査。
 *
 * **これは落ちないバグを探す検査。** 保存していなければ fork も write も
 * 成功したまま、計算結果だけが静かに変わる。症状が出てから追うと高くつくので、
 * fork を入れた時点で検査ごと用意する。
 *
 * ---- なぜ d8-d15 を見るか ------------------------------------------------
 *
 * AArch64 の呼び出し規約では **v8-v15 の下 64bit だけが callee-saved**。
 * v0-v7 / v16-v31 は関数を 1 つ呼べば壊れてよいレジスタなので、
 * write() を挟んだ後に値が残っている保証がない。**規約上壊れてよいものを
 * 検査に使うと、カーネルが正しくても落ちる検査になる。**
 *
 * switch.S は 32 本すべてを退避するが、ユーザー空間から検証できるのは
 * この 8 本。ここが保たれていれば「切り替えで FP を持ち越している」ことは
 * 確かめられる。
 *
 * ---- 何を再現するか ------------------------------------------------------
 *
 * fork して親子が別の値を d8/d9 に入れ、**互いに走り続けながら**繰り返し
 * 照合する。保存していなければ、切り替わった瞬間に相手の値が見える。
 */
#include <stddef.h>
#include <stdint.h>
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

/* callee-saved の FP レジスタに 64bit の値を置く */
static void fp_set(uint64_t a, uint64_t b) {
    __asm__ volatile("fmov d8, %0\n\t"
                     "fmov d9, %1"
                     :
                     : "r"(a), "r"(b)
                     : "d8", "d9");
}

static void fp_get(uint64_t* a, uint64_t* b) {
    __asm__ volatile("fmov %0, d8\n\t"
                     "fmov %1, d9"
                     : "=r"(*a), "=r"(*b));
}

/* **切り替えを確実に跨がせる。** タイマ割り込みでプリエンプトされるだけの
 * 時間を、syscall を呼ばずに消費する (syscall を挟むと ABI 上 caller-saved の
 * レジスタが壊れてよいので、検査の意図がぼやける) */
static void spin(volatile uint64_t n) {
    while (n--) __asm__ volatile("" ::: "memory");
}

#define ROUNDS   40
#define SPIN_N   200000

int main(void) {
    pid_t pid;
    int status = 0;
    uint64_t want_a, want_b, got_a, got_b;

    if (write_all(1, "FP-START\n", 9) < 0) return 10;

    pid = fork();
    if (pid < 0) return 11;

    if (pid == 0) {
        /* ---- 子 ---- */
        want_a = 0xC1C1C1C1C1C1C1C1ULL;
        want_b = 0xD2D2D2D2D2D2D2D2ULL;
        fp_set(want_a, want_b);
        for (int i = 0; i < ROUNDS; i++) {
            spin(SPIN_N);
            fp_get(&got_a, &got_b);
            if (got_a != want_a || got_b != want_b) {
                write_all(1, "FP-CHILD-CLOBBERED\n", 19);
                _exit(12);
            }
        }
        write_all(1, "FP-CHILD-OK\n", 12);
        _exit(0);
    }

    /* ---- 親 ---- */
    want_a = 0xA5A5A5A5A5A5A5A5ULL;
    want_b = 0xB6B6B6B6B6B6B6B6ULL;
    fp_set(want_a, want_b);
    for (int i = 0; i < ROUNDS; i++) {
        spin(SPIN_N);
        fp_get(&got_a, &got_b);
        if (got_a != want_a || got_b != want_b) {
            write_all(1, "FP-PARENT-CLOBBERED\n", 20);
            return 13;
        }
    }
    if (write_all(1, "FP-PARENT-OK\n", 13) < 0) return 14;

    if (waitpid(pid, &status, 0) < 0) return 15;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        write_all(1, "FP-CHILD-BAD\n", 13);
        return 16;
    }

    if (write_all(1, "FP-DONE\n", 8) < 0) return 17;
    return 0;
}
