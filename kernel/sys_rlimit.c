/* ---- getrlimit / prlimit64 (3 アーキ共通) ---------------------------------
 *
 * **別実装 29 組のうちの 1 組を畳んだもの (2026-09-13)。**もとは
 *
 *     x86              kernel/x86_64/sys_time.c の sys_getrlimit / sys_prlimit64
 *     aarch64/riscv64  kernel/linux_syscall.c   の linux_sys_getrlimit /
 *                                                  linux_sys_prlimit64
 *
 * の 2 つがあり、**重複ではなく答える値が違っていた。**
 *
 * | | x86 | linux 側 |
 * |---|---|---|
 * | resource        | 見ていない (全部 無限) | 種類ごとに答える |
 * | 範囲外の resource | 成功を返す            | EINVAL |
 * | NOFILE          | 無限                  | MAX_FDS |
 * | STACK           | 無限                  | 実際に張っている大きさ |
 * | CORE            | 無限                  | 0 |
 *
 * **linux 側に寄せた。**理由は向こうのコメントに書いてあったとおりで、
 * どちらも「無限と答えると呼び手が困る」という実害がある:
 *
 *   NOFILE  無限にすると、呼び手が上限まで close() を回して大量に空振りする
 *   STACK   無限だと alloca を使う側が踏み外す
 *
 * x86 はこれまで全部 無限で答えていたので、この 1 組を畳むと**x86 の
 * 振る舞いが変わる (直る) 側**になる。 */

#include <stdint.h>
#include "fs.h"              /* MAX_FDS */
#include "pmm.h"             /* PAGE_SIZE */
#include "linux_abi.h"       /* struct linux_rlimit */
#include "linux_errno.h"
#include "linux_syscalls.h"  /* LINUX_RLIMIT_* / LINUX_RLIM_INFINITY */
#include "sys_internal.h"
#include "task.h"

/* **無限で答えてよいものと、実際の値を答えるべきものがある。** */
static void rlimit_for(int resource, struct linux_rlimit* out) {
    struct task* current = get_current_task();
    out->rlim_cur = LINUX_RLIM_INFINITY;
    out->rlim_max = LINUX_RLIM_INFINITY;
    switch (resource) {
        case LINUX_RLIMIT_NOFILE:
            out->rlim_cur = MAX_FDS;
            out->rlim_max = MAX_FDS;
            break;
        case LINUX_RLIMIT_STACK:
            /* **実際に張ってある大きさを答える。** USER_STACK_PAGES は
             * kernel/task_internal.h にあり共有層からは見えないので、
             * タスクが持っている上端と下端の差から出す */
            if (current && current->user_stack_top > current->user_stack_bottom) {
                out->rlim_cur = current->user_stack_top - current->user_stack_bottom;
            } else {
                out->rlim_cur = 64ULL * PAGE_SIZE;   /* 張る前に聞かれたとき */
            }
            out->rlim_max = out->rlim_cur;
            break;
        case LINUX_RLIMIT_CORE:
            /* コアダンプは出さない */
            out->rlim_cur = 0;
            out->rlim_max = 0;
            break;
        default:
            break;
    }
}

int sys_getrlimit(int resource, struct linux_rlimit* rlim) {
    if (resource < 0 || resource >= LINUX_RLIMIT_NLIMITS) return -LINUX_EINVAL;
    if (!rlim) return -LINUX_EFAULT;
    rlimit_for(resource, rlim);
    return 0;
}

/* 上限は記憶しないが、**成功を返す**。失敗にすると呼び手が落ちる */
int sys_setrlimit(int resource, const struct linux_rlimit* rlim) {
    if (resource < 0 || resource >= LINUX_RLIMIT_NLIMITS) return -LINUX_EINVAL;
    if (!rlim) return -LINUX_EFAULT;
    return 0;
}

int sys_prlimit64(int pid, int resource, const struct linux_rlimit* new_limit,
                  struct linux_rlimit* old_limit) {
    struct task* current = get_current_task();
    if (resource < 0 || resource >= LINUX_RLIMIT_NLIMITS) return -LINUX_EINVAL;
    /* pid 0 は自分。他プロセスは見ない。**自分以外の上限は扱えない**ので
     * Linux と同じく EPERM を返す */
    if (pid != 0 && current && pid != current->pid) return -LINUX_EPERM;
    if (old_limit) rlimit_for(resource, old_limit);
    (void)new_limit;   /* 記憶しない (上のコメント) */
    return 0;
}
