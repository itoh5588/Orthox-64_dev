#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "sound.h"
#include "spinlock.h"
#include "linux_abi.h"     /* LINUX_SIGCHLD。x86 だけ 20 を使っていた */
#include "linux_errno.h"
#include "sys_internal.h"
#include "syscall.h"
#include "task.h"


extern struct task* task_list;
extern void puts(const char* s);

int sys_arch_prctl(int code, uint64_t addr) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;

    switch (code) {
        case ARCH_SET_FS:
            current->user_fs_base = addr;
            /* **同じ MSR を書く hook が既に在る** (include/x86_64/task.h の
             * arch_task_apply_user_tls)。ここに wrmsr を直書きしていたのは
             * 重複で、**x86 の命令が syscall 層に居た**。2026-09-07 に寄せた */
            arch_task_apply_user_tls(addr);
            return 0;
        case ARCH_GET_FS:
            *(uint64_t*)addr = current->user_fs_base;
            return 0;
        default:
            return -LINUX_EINVAL;
    }
}

/* **返り値は Linux ABI に揃えてある (2026-09-08)。**以前は -1 (= EPERM) と
 * -11 の直書きだった。aarch64 / riscv64 側 (linux_syscall.c) は同じロジックで
 * 正しい errno を返しており、**musl は futex の errno を見て動きを変える**ので
 * x86 だけ EPERM を返す理由が無い。 */

int sys_set_robust_list(const void* head, size_t len) {
    (void)head;
    (void)len;
    return 0;
}


/* **task_list は kernel/task.c の task_signal_pid に任せる (2026-09-20)。**
 *
 * 以前はここで BKL を持っているかだけを見て task_list を辿っていた。
 * **BKL は task_list を守るロックではない** —— 守るのは g_task_lock で、
 * 別の CPU が回収でノードを外すと巡回が壊れる。31429f9 で他の 6 か所を
 * ロックの中へ移したとき、ここだけ残っていた (日報2026-09-19 §9-7)。
 *
 * 見つける・立てる・zombie にする・親に知らせるの間でロックを放さないので、
 * その隙に相手が回収される窓も無くなる。 */
int sys_kill(int pid, int sig) {
    struct task* current = get_current_task();

    if (pid == 0) {
        /* 自分自身。task_list を辿らないので、そのまま立てる */
        if (!current) return -LINUX_ESRCH;
        pid = current->pid;
    }
    if (pid <= 0) return 0;

    /* 終わらせる種類かどうかは従来どおり (SIGINT / SIGTERM / SIGKILL) */
    int terminate = (sig == 2 || sig == 15 || sig == 9);
    int ret = task_signal_pid(pid, sig, terminate, 128 + sig, LINUX_SIGCHLD);
    return (ret == 0) ? 0 : -LINUX_ESRCH;
}

int sys_getpgrp(void) {
    struct task* current = get_current_task();
    return current ? current->pgid : -LINUX_ESRCH;
}

int sys_setpgid(int pid, int pgid) {
    struct task* current = get_current_task();
    struct task* target = 0;
    if (!current) return -LINUX_ESRCH;
    if (pid == 0) pid = current->pid;
    if (pgid == 0) pgid = pid;
    (void)target;
    /* **巡回も書き換えもロックの中** (訳は sys_kill の上のコメント) */
    return (task_set_pgid(pid, pgid) == 0) ? 0 : -LINUX_ESRCH;
}

int sys_setsid(void) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    /* **既にプロセスグループリーダーなら EPERM。**Linux の規定で、
     * 生の -1 が偶然この値だったが意図が読めなかった */
    if (current->pgid == current->pid) return -LINUX_EPERM;
    current->sid = current->pid;
    current->pgid = current->pid;
    tty_pgrp_set(current->pgid);
    return current->sid;
}
