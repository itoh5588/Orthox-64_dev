#include <stddef.h>
#include <stdint.h>
#include "fs.h"
#include "sound.h"
#include "spinlock.h"
#include "sys_internal.h"
#include "syscall.h"
#include "task.h"

#define MSR_FS_BASE 0xC0000100

extern struct task* task_list;
extern void puts(const char* s);

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

uint64_t sys_getpid(void) {
    return (uint64_t)get_current_task()->pid;
}

uint64_t sys_getppid(void) {
    struct task* current = get_current_task();
    return (uint64_t)(current ? current->ppid : 0);
}

uint64_t sys_getuid(void) {
    return 0;
}

uint64_t sys_getgid(void) {
    return 0;
}

uint64_t sys_geteuid(void) {
    return 0;
}

uint64_t sys_getegid(void) {
    return 0;
}

int sys_arch_prctl(int code, uint64_t addr) {
    struct task* current = get_current_task();
    if (!current) return -1;

    switch (code) {
        case ARCH_SET_FS:
            current->user_fs_base = addr;
            wrmsr(MSR_FS_BASE, addr);
            return 0;
        case ARCH_GET_FS:
            *(uint64_t*)addr = current->user_fs_base;
            return 0;
        default:
            return -1;
    }
}

int sys_futex(volatile int* uaddr, int op, int val) {
    int cmd = op & ~FUTEX_PRIVATE;
    if (!uaddr) return -1;

    switch (cmd) {
        case FUTEX_WAIT:
            /* Single-threaded for now: only validate the expected value. */
            return (*uaddr == val) ? 0 : -11;
        case FUTEX_WAKE:
            return 0;
        default:
            return -1;
    }
}

int sys_set_robust_list(const void* head, size_t len) {
    (void)head;
    (void)len;
    return 0;
}

int sys_set_tid_address(int* tidptr) {
    struct task* current = get_current_task();
    (void)tidptr;
    return current ? current->pid : -1;
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

static int task_signal_pending(const struct task* t, int sig) {
    if (!t || sig <= 0 || sig >= 64) return 0;
    return (t->sig_pending & (1ULL << sig)) != 0;
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
    task_mark_zombie(current, status);
    if (current->ppid > 0) {
        struct task* parent = find_task_by_pid_locked(current->ppid);
        if (parent) task_signal_add_locked(parent, 20);
    }
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
                        current->sig_pending &= ~(1ULL << 20);
                        (void)task_reap(curr);
                        return child_pid;
                    }
                }
            }
            curr = curr->next;
        }
        if (!found_child) return -1;
        /* WNOHANG: 生きている子はいるがゾンビ無し → ブロックせず 0。
         * これを無視すると make -j の非ブロッキング reap が子の終了まで
         * 眠り、並列ジョブ投入が完全に直列化する (実測で確認)。 */
        if (options & ORTH_WNOHANG) return 0;
        task_mark_sleeping(current);
        if (task_signal_pending(current, 20)) {
            if (current->state == TASK_SLEEPING) {
                task_wake(current);
            }
            continue;
        }
        kernel_yield();
    }
}

int sys_kill(int pid, int sig) {
    struct task* current = get_current_task();
    struct task* t = 0;
    if (sig == 0) {
        if (pid > 0) return find_task_by_pid_locked(pid) ? 0 : -1;
        return 0;
    }
    if (pid > 0) {
        t = find_task_by_pid_locked(pid);
    } else if (pid == 0 && current) {
        t = current;
    }
    if (!t) return -1;
    task_signal_add_locked(t, sig);
    if (sig == 2 || sig == 15 || sig == 9) {
        task_mark_zombie(t, 128 + sig);
        if (t->ppid > 0) {
            struct task* parent = find_task_by_pid_locked(t->ppid);
            if (parent) task_signal_add_locked(parent, 20);
        }
        return 0;
    }
    return 0;
}

static int g_tty_pgrp = 0;

int sys_getpgrp(void) {
    struct task* current = get_current_task();
    return current ? current->pgid : -1;
}

int sys_setpgid(int pid, int pgid) {
    struct task* current = get_current_task();
    struct task* target = 0;
    if (!current) return -1;
    if (pid == 0) pid = current->pid;
    if (pgid == 0) pgid = pid;
    target = find_task_by_pid_locked(pid);
    if (!target) return -1;
    target->pgid = pgid;
    return 0;
}

int sys_setsid(void) {
    struct task* current = get_current_task();
    if (!current) return -1;
    if (current->pgid == current->pid) return -1;
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
