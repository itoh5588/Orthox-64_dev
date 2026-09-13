/* ---- プロセス層の syscall (3 アーキ共通) ----------------------------------
 *
 * **別実装 29 組を畳んだものを置く場所 (2026-09-13 から)。**もとは
 *
 *     x86              kernel/sys_proc.c      (aarch64 / riscv64 は組んでいない)
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_*
 *
 * に同じ syscall が 2 つずつあった。**kernel/sys_proc.c をまるごと共有層へ
 * 移すことはできない** —— あの中には x86 だけが持つもの (arch_prctl など) や、
 * linux 側と振る舞いが違うもの (exit / wait4 / kill) が混ざっているため。
 * 突き合わせが済んだものからここへ 1 組ずつ移す。 */

#include <stddef.h>
#include <stdint.h>
#include "linux_errno.h"
#include "syscall.h"        /* FUTEX_WAIT / FUTEX_WAKE / FUTEX_PRIVATE */
#include "sys_internal.h"
#include "task.h"

/* ---- futex ----------------------------------------------------------------
 * **待ちは実装していない。**値が合っているかだけを見る (単一スレッド前提)。
 * 2026-09-08 に x86 側の返り値を linux 側へ揃えてあり、畳んだ時点で
 * 両者は同じものだった。
 *
 *   uaddr が NULL       EFAULT
 *   FUTEX_WAIT          値が違えば EAGAIN、同じなら 0
 *   FUTEX_WAKE          0
 *   それ以外            **ENOSYS。**EPERM だと呼び手が「権限が無い」と誤解する */
int sys_futex(volatile int* uaddr, int op, int val) {
    int cmd = op & ~FUTEX_PRIVATE;
    if (!uaddr) return -LINUX_EFAULT;

    switch (cmd) {
        case FUTEX_WAIT:
            return (*uaddr == val) ? 0 : -LINUX_EAGAIN;
        case FUTEX_WAKE:
            return 0;
        default:
            return -LINUX_ENOSYS;
    }
}

/* ---- set_tid_address ------------------------------------------------------
 * **場所は憶えない。**返すのは自分の tid だけ。current が 0 になるのは
 * カーネル内部の異常で、ここには来ない */
int sys_set_tid_address(int* tidptr) {
    struct task* current = get_current_task();
    (void)tidptr;
    return current ? current->pid : -LINUX_ESRCH;
}

/* ---- getpid 族 ------------------------------------------------------------
 * **x86 の getpid だけ NULL 検査が無かった** (get_current_task()->pid を直に
 * 引いていた)。linux 側は current が 0 なら 0 を返していたので、そちらに
 * 寄せた。残りの 5 つは両側で同じだった。 */
uint64_t sys_getpid(void) {
    struct task* current = get_current_task();
    return current ? (uint64_t)current->pid : 0;
}

uint64_t sys_getppid(void) {
    struct task* current = get_current_task();
    return current ? (uint64_t)current->ppid : 0;
}

/* **単一ユーザー (root 固定)。** /etc/passwd も root だけを持つ */
uint64_t sys_getuid(void)  { return 0; }
uint64_t sys_getgid(void)  { return 0; }
uint64_t sys_geteuid(void) { return 0; }
uint64_t sys_getegid(void) { return 0; }
