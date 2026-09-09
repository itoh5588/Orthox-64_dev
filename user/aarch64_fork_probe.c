/*
 * P3-1: fork が aarch64 で成立するかの検査。
 *
 * P2 の probe (user/aarch64_musl_probe.c) から fork を外していたのは、
 * arch_vm_clone_address_space が 0 を返し、aarch64_task_fork_child_return が
 * `b .` で止まっていたため。両方を入れたので、ここで確かめる。
 *
 * **「fork が返った」だけでは足りない。** 確かめたいのは 3 つ:
 *
 *   1. 子が EL0 で走り出す        fork_child_return が eret まで届いている
 *   2. 親が waitpid で回収できる   子の終了ステータスが親に届く
 *   3. **親子のメモリが独立している**
 *      arch_vm_clone_address_space はページを実コピーする (CoW ではない)。
 *      写せていなければ親子が同じ物理ページを共有し、**どちらの書き込みも
 *      相手に見える**。fork も waitpid も成功したまま、データだけが壊れる。
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

int main(void) {
    pid_t pid;
    int status = 0;

    if (write_all(1, "FORK-START\n", 11) < 0) return 10;

    /* 親が先に書いておく。子はこれを読めるはず (fork 時点の写し) */
    memcpy((void*)g_shared_probe, "PARENT-WROTE-THIS", 18);

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

    if (write_all(1, "FORK-DONE\n", 10) < 0) return 22;
    return 0;
}
