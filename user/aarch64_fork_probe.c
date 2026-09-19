/*
 * P3-1: fork が aarch64 で成立するかの検査。
 *
 * P2 の probe (user/aarch64_musl_probe.c) から fork を外していたのは、
 * arch_vm_clone_address_space が 0 を返し、aarch64_task_fork_child_return が
 * `b .` で止まっていたため。両方を入れたので、ここで確かめる。
 *
 * **「fork が返った」だけでは足りない。** 確かめたいのは 4 つ:
 *
 *   1. 子が EL0 で走り出す        fork_child_return が eret まで届いている
 *   2. 親が waitpid で回収できる   子の終了ステータスが親に届く
 *   3. **親子のメモリが独立している**
 *      fork は親子でページを共有し、先に書いた側が写す (CoW, 2026-09-19。
 *      それまではその場で全ページを写していた)。写せていなければ
 *      **どちらの書き込みも相手に見える**。fork も waitpid も成功したまま、
 *      データだけが壊れる。
 *   4. **カーネルが書いても写る** (2026-09-19)
 *      read(2) はカーネル (EL1) がユーザーのバッファへ直接書く。共有中の
 *      ページだと EL1 の permission fault になり、そこでも写す必要がある
 *      (kernel/aarch64/boot.c の aarch64_sync_exception)。子が一度も
 *      触っていないページへ read させて、この経路だけを通す。
 */
#include <stddef.h>
#include <string.h>
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

/* **bss に置く。** スタックだと親子で別ページになりやすく、
 * 「たまたま分かれていた」だけで通ってしまう */
static volatile char g_shared_probe[64];

/* **4. の確かめ用。ページを丸ごと専有させる。**g_shared_probe と同じ
 * ページだと、子がそちらへ書いた時点で (EL0 のフォルトで) 写ってしまい、
 * EL1 の経路を通らない */
static char g_kwrite_probe[4096] __attribute__((aligned(4096)));

int main(void) {
    pid_t pid;
    int status = 0;

    if (write_all(1, "FORK-START\n", 11) < 0) return 10;

    /* 親が先に書いておく。子はこれを読めるはず (fork 時点の写し) */
    memcpy((void*)g_shared_probe, "PARENT-WROTE-THIS", 18);
    memcpy(g_kwrite_probe, "PARENT-KWRITE", 14);

    pid = fork();
    if (pid < 0) return 11;

    if (pid == 0) {
        /* ---- 子 ---- */
        if (write_all(1, "FORK-CHILD\n", 11) < 0) _exit(12);
        /* 親が書いた内容が見えること = 空間が正しく写っている */
        if (memcmp((void*)g_shared_probe, "PARENT-WROTE-THIS", 18) != 0) _exit(13);
        if (write_all(1, "FORK-CHILD-SEES-PARENT\n", 23) < 0) _exit(14);
        /* **子だけが書き換える。** 親に見えたら空間が共有されている */
        memcpy((void*)g_shared_probe, "CHILD-OVERWROTE!!", 18);
        if (write_all(1, "FORK-CHILD-WROTE\n", 17) < 0) _exit(15);

        /* **4. カーネルに書かせる。**g_kwrite_probe には子から一度も
         * 触らない。pipe に流したものを read で直接受ける */
        {
            int fds[2];
            if (pipe(fds) < 0) _exit(23);
            if (write_all(fds[1], "CHILD-READ-KWRITE", 18) < 0) _exit(24);
            if (read(fds[0], g_kwrite_probe, 18) != 18) _exit(25);
            if (memcmp(g_kwrite_probe, "CHILD-READ-KWRITE", 18) != 0) _exit(26);
            if (write_all(1, "FORK-CHILD-KWRITE\n", 18) < 0) _exit(27);
        }
        _exit(0);
    }

    /* ---- 親 ---- */
    if (waitpid(pid, &status, 0) < 0) return 16;
    if (!WIFEXITED(status)) return 17;
    if (WEXITSTATUS(status) != 0) return 18;
    if (write_all(1, "FORK-REAPED\n", 12) < 0) return 19;

    /* **ここが本命。** 子の書き込みが見えていたら、ページを写せていない */
    if (memcmp((void*)g_shared_probe, "PARENT-WROTE-THIS", 18) != 0) {
        write_all(1, "FORK-SHARED-BAD\n", 16);
        return 20;
    }
    if (write_all(1, "FORK-ISOLATED\n", 14) < 0) return 21;

    /* 4. 子の read が親のページに入っていたら、EL1 の経路で写せていない */
    if (memcmp(g_kwrite_probe, "PARENT-KWRITE", 14) != 0) {
        write_all(1, "FORK-KWRITE-SHARED-BAD\n", 23);
        return 28;
    }
    if (write_all(1, "FORK-KWRITE-ISOLATED\n", 21) < 0) return 29;

    if (write_all(1, "FORK-DONE\n", 10) < 0) return 22;
    return 0;
}
