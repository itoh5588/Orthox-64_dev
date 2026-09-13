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


static struct task* find_task_by_pid_locked(int pid) {
    if (!kernel_lock_held()) {
        puts("[warn] find_task_by_pid_locked without BKL\r\n");
        return 0;
    }
    struct task* t = task_list;
    while (t) {
        if (t->pid == pid) return t;
        t = t->next;
    }
    return 0;
}

static void task_signal_add_locked(struct task* t, int sig) {
    if (!kernel_lock_held()) {
        puts("[warn] task_signal_add_locked without BKL\r\n");
        return;
    }
    if (!t || sig <= 0 || sig >= 64) return;
    if (sig < 32 && t->sig_handlers[sig] == 1ULL) return;
    t->sig_pending |= (1ULL << sig);
    if (t->state == TASK_SLEEPING) {
        task_wake(t);
    }
}


void sys_exit(int status) {
    struct task* current = get_current_task();
    // Ensure no speaker tone leaks when a user task exits abruptly.
    sound_beep_stop();
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd].in_use) {
            (void)fs_close(fd);
        }
    }
    /* **自分の子を始末してから zombie になる。**入っていなかったので、
     * 親のいない zombie が溜まっていた (2026-09-08 に実測)。aarch64 /
     * riscv64 は 2026-08-31 から同じことをしている */
    task_reap_orphans_of(current->pid);
    task_mark_zombie(current, status);
    if (current->ppid > 0) {
        struct task* parent = find_task_by_pid_locked(current->ppid);
        if (parent) task_signal_add_locked(parent, LINUX_SIGCHLD);
    }
    /* **待ち行列で寝ている親を起こす (2026-09-09)。**これが無いと親は
     * 時間切れ (TASK_CHILD_WAIT_POLL_MS) まで気づかない */
    task_child_exit_wake();
    while (1) kernel_yield();
}

#define ORTH_WNOHANG 1

int64_t sys_wait4(int pid, int* wstatus, int options) {
    struct task* current = get_current_task();
    while (1) {
        int found_child = 0;
        struct task* curr = task_list;
        while (curr) {
            if (curr->ppid == current->pid) {
                if (pid == -1 || curr->pid == pid) {
                    found_child = 1;
                    if (curr->state == TASK_ZOMBIE) {
                        int child_pid = curr->pid;
                        if (wstatus) *wstatus = curr->exit_status << 8;
                        current->sig_pending &= ~(1ULL << LINUX_SIGCHLD);
                        (void)task_reap(curr);
                        return child_pid;
                    }
                }
            }
            curr = curr->next;
        }
        if (!found_child) return -LINUX_ECHILD;
        /* WNOHANG: 生きている子はいるがゾンビ無し → ブロックせず 0。
         * これを無視すると make -j の非ブロッキング reap が子の終了まで
         * 眠り、並列ジョブ投入が完全に直列化する (実測で確認)。 */
        if (options & ORTH_WNOHANG) return 0;
        /* **焼かずに寝る (2026-09-09)。**ここは 2026-08-30 まで
         * aarch64 / riscv64 と同じく kernel_yield() で回しており、
         * **待っている親がコアを 1 本 100% 使っていた。**あちらだけ直って
         * いたので、実装を kernel/task.c へ出して両方から呼ぶ。
         * 子の exit で起こされるか、遅くとも時間切れで自力で起きる */
        task_wait_child_exit(current->pid, pid, TASK_CHILD_WAIT_POLL_MS);
    }
}

int sys_kill(int pid, int sig) {
    struct task* current = get_current_task();
    struct task* t = 0;
    if (sig == 0) {
        if (pid > 0) return find_task_by_pid_locked(pid) ? 0 : -LINUX_ESRCH;
        return 0;
    }
    if (pid > 0) {
        t = find_task_by_pid_locked(pid);
    } else if (pid == 0 && current) {
        t = current;
    }
    if (!t) return -LINUX_ESRCH;
    task_signal_add_locked(t, sig);
    if (sig == 2 || sig == 15 || sig == 9) {
        task_mark_zombie(t, 128 + sig);
        if (t->ppid > 0) {
            struct task* parent = find_task_by_pid_locked(t->ppid);
            if (parent) task_signal_add_locked(parent, LINUX_SIGCHLD);
        }
        return 0;
    }
    return 0;
}

static int g_tty_pgrp = 0;

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
    target = find_task_by_pid_locked(pid);
    if (!target) return -LINUX_ESRCH;
    target->pgid = pgid;
    return 0;
}

int sys_setsid(void) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    /* **既にプロセスグループリーダーなら EPERM。**Linux の規定で、
     * 生の -1 が偶然この値だったが意図が読めなかった */
    if (current->pgid == current->pid) return -LINUX_EPERM;
    current->sid = current->pid;
    current->pgid = current->pid;
    g_tty_pgrp = current->pgid;
    return current->sid;
}

int sys_tcgetpgrp(int fd) {
    (void)fd;
    if (g_tty_pgrp == 0) {
        struct task* current = get_current_task();
        if (current) g_tty_pgrp = current->pgid;
    }
    return g_tty_pgrp;
}

int sys_tcsetpgrp(int fd, int pgrp) {
    (void)fd;
    g_tty_pgrp = pgrp;
    return 0;
}

int tty_get_foreground_pgrp(void) {
    if (g_tty_pgrp == 0) {
        struct task* current = get_current_task();
        if (current) g_tty_pgrp = current->pgid;
    }
    return g_tty_pgrp;
}
