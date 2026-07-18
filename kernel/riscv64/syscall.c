#include <stdint.h>
#include "fs.h"
#include "pmm.h"
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/time.h"
#include "spinlock.h"
#include "riscv64/syscall.h"
#include "syscall.h"
#include "task.h"

static task_context_t* g_riscv64_fallback_current_context;
extern int task_fork(arch_task_exec_frame_t* frame);
extern struct task* task_list;
static int64_t riscv64_bootstrap_sys_wait4(int pid, int* wstatus, int options);

#define RISCV64_USER_MMAP_BASE_VADDR 0x0000002000000000ULL

#define RISCV64_LINUX_SYS_IOCTL            29
#define RISCV64_LINUX_SYS_GETCWD           17
#define RISCV64_LINUX_SYS_OPENAT           56
#define RISCV64_LINUX_SYS_CLOSE            57
#define RISCV64_LINUX_SYS_GETDENTS64       61
#define RISCV64_LINUX_SYS_LSEEK            62
#define RISCV64_LINUX_SYS_READ             63
#define RISCV64_LINUX_SYS_WRITE            64
#define RISCV64_LINUX_SYS_READV            65
#define RISCV64_LINUX_SYS_WRITEV           66
#define RISCV64_LINUX_SYS_NEWFSTATAT       79
#define RISCV64_LINUX_SYS_FSTAT            80
#define RISCV64_LINUX_SYS_EXIT             93
#define RISCV64_LINUX_SYS_EXIT_GROUP       94
#define RISCV64_LINUX_SYS_WAITID           95
#define RISCV64_LINUX_SYS_SET_TID_ADDRESS  96
#define RISCV64_LINUX_SYS_FUTEX            98
#define RISCV64_LINUX_SYS_CLOCK_GETTIME    113
#define RISCV64_LINUX_SYS_RT_SIGACTION     134
#define RISCV64_LINUX_SYS_RT_SIGPROCMASK   135
#define RISCV64_LINUX_SYS_GETPID           172
#define RISCV64_LINUX_SYS_BRK              214
#define RISCV64_LINUX_SYS_MUNMAP           215
#define RISCV64_LINUX_SYS_CLONE            220
#define RISCV64_LINUX_SYS_MMAP             222
#define RISCV64_LINUX_SYS_WAIT4            260
#define RISCV64_LINUX_SYS_GETRANDOM        278

#define RISCV64_LINUX_AT_EMPTY_PATH        0x1000
#define RISCV64_LINUX_TCGETS               0x5401UL
#define RISCV64_LINUX_TCSETS               0x5402UL
#define RISCV64_LINUX_TIOCGPGRP            0x540FUL
#define RISCV64_LINUX_TIOCSPGRP            0x5410UL
#define RISCV64_LINUX_TIOCGWINSZ           0x5413UL

#define RISCV64_LINUX_SIG_BLOCK   0
#define RISCV64_LINUX_SIG_UNBLOCK 1
#define RISCV64_LINUX_SIG_SETMASK 2

struct riscv64_linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct riscv64_linux_iovec {
    void* iov_base;
    size_t iov_len;
};

struct riscv64_linux_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

struct riscv64_linux_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    uint8_t payload[112];
};

struct riscv64_linux_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct riscv64_linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static int g_riscv64_tty_pgrp;
static struct riscv64_linux_termios g_riscv64_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

static uint64_t riscv64_align_up_page(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static int64_t riscv64_bootstrap_sys_write(int fd, const void* buf, size_t count) {
    return sys_write(fd, buf, count);
}

static int64_t riscv64_bootstrap_sys_lseek(int fd, int64_t offset, int whence) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    int64_t base;
    int64_t next;

    if (!current) return -1;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -1;
    f = &current->fds[fd];
    if (f->type == FT_DIR) return -1;

    switch (whence) {
        case 0:
            base = 0;
            break;
        case 1:
            base = (int64_t)f->offset;
            break;
        case 2:
            base = (int64_t)f->size;
            break;
        default:
            return -1;
    }

    next = base + offset;
    if (next < 0 || (uint64_t)next > f->size) return -1;
    f->offset = (size_t)next;
    return next;
}

static int64_t riscv64_bootstrap_sys_writev(int fd, const struct riscv64_linux_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = riscv64_bootstrap_sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

static int64_t riscv64_bootstrap_sys_readv(int fd, const struct riscv64_linux_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (!iov || iovcnt < 0) return -1;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

static uint64_t riscv64_bootstrap_sys_brk(uint64_t addr) {
    struct task* current = get_current_task();
    uint64_t current_page;
    uint64_t target_page;
    arch_address_space_t address_space;

    if (!current) return 0;
    if (addr == 0 || addr <= current->heap_break) return current->heap_break;

    current_page = riscv64_align_up_page(current->heap_break);
    target_page = riscv64_align_up_page(addr);
    address_space = arch_task_context_get_address_space(&current->ctx);

    while (current_page < target_page) {
        uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc(1);
        if (!phys) return current->heap_break;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) {
            ((uint8_t*)(uintptr_t)phys)[i] = 0;
        }
        arch_vm_map_page(address_space, current_page, phys, arch_vm_user_page_flags(1, 0));
        current_page += PAGE_SIZE;
    }

    current->heap_break = addr;
    riscv64_sfence_vma();
    return current->heap_break;
}

static int riscv64_bootstrap_sys_munmap(void* addr, size_t length) {
    struct task* current = get_current_task();
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t size = riscv64_align_up_page((uint64_t)length);
    arch_address_space_t address_space;

    if (!current || !addr || length == 0) return -1;
    if ((base & (PAGE_SIZE - 1ULL)) != 0) return -1;

    address_space = arch_task_context_get_address_space(&current->ctx);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        arch_vm_unmap_page(address_space, base + off);
    }
    riscv64_sfence_vma();
    return 0;
}

static int riscv64_bootstrap_sys_set_tid_address(int* tidptr) {
    struct task* current = get_current_task();
    (void)tidptr;
    return current ? current->pid : -1;
}

static int riscv64_bootstrap_sys_futex(volatile int* uaddr, int op, int val) {
    int cmd = op & ~FUTEX_PRIVATE;
    if (!uaddr) return -1;
    switch (cmd) {
        case FUTEX_WAIT:
            return (*uaddr == val) ? 0 : -11;
        case FUTEX_WAKE:
            return 0;
        default:
            return -1;
    }
}

static int riscv64_bootstrap_sys_clock_gettime(int clock_id, struct riscv64_linux_timespec* ts) {
    uint64_t ms;
    if (!ts) return -1;
    ms = arch_time_now_ms();
    if (clock_id != 0 && clock_id != 1) return -1;
    ts->tv_sec = (int64_t)(ms / 1000ULL);
    ts->tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL);
    return 0;
}

static int64_t riscv64_bootstrap_sys_ioctl(int fd, unsigned long request, uint64_t arg) {
    switch (request) {
        case RISCV64_LINUX_TIOCGWINSZ:
            if (!arg) return -1;
            ((struct riscv64_linux_winsize*)(uintptr_t)arg)->ws_row = 25;
            ((struct riscv64_linux_winsize*)(uintptr_t)arg)->ws_col = 80;
            ((struct riscv64_linux_winsize*)(uintptr_t)arg)->ws_xpixel = 0;
            ((struct riscv64_linux_winsize*)(uintptr_t)arg)->ws_ypixel = 0;
            return 0;
        case RISCV64_LINUX_TIOCGPGRP:
            if (!arg) return -1;
            if (g_riscv64_tty_pgrp == 0) {
                struct task* current = get_current_task();
                if (current) g_riscv64_tty_pgrp = current->pgid;
            }
            *(int*)(uintptr_t)arg = g_riscv64_tty_pgrp;
            return 0;
        case RISCV64_LINUX_TIOCSPGRP:
            if (!arg) return -1;
            g_riscv64_tty_pgrp = *(const int*)(uintptr_t)arg;
            return 0;
        case RISCV64_LINUX_TCGETS:
            if (!arg) return -1;
            *(struct riscv64_linux_termios*)(uintptr_t)arg = g_riscv64_console_termios;
            return 0;
        case RISCV64_LINUX_TCSETS:
            if (!arg) return -1;
            g_riscv64_console_termios = *(const struct riscv64_linux_termios*)(uintptr_t)arg;
            return 0;
        default:
            (void)fd;
            return -25;
    }
}

static int riscv64_bootstrap_sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize) {
    struct task* current = get_current_task();
    uint64_t newmask;
    if (!current) return -1;
    if (sigsetsize != sizeof(uint64_t)) return -1;
    if (oldset) *oldset = current->sig_mask;
    if (!set) return 0;
    newmask = *set;
    switch (how) {
        case RISCV64_LINUX_SIG_BLOCK:
            current->sig_mask |= newmask;
            break;
        case RISCV64_LINUX_SIG_UNBLOCK:
            current->sig_mask &= ~newmask;
            break;
        case RISCV64_LINUX_SIG_SETMASK:
            current->sig_mask = newmask;
            break;
        default:
            return -1;
    }
    return 0;
}

static int riscv64_bootstrap_sys_rt_sigaction(int sig, const struct riscv64_linux_sigaction* act,
                                              struct riscv64_linux_sigaction* oldact, size_t sigsetsize) {
    struct task* current = get_current_task();
    if (!current || sig <= 0 || sig >= 32) return -1;
    if (sigsetsize != sizeof(uint64_t)) return -1;
    if (oldact) {
        oldact->sa_handler = current->sig_handlers[sig];
        oldact->sa_flags = current->sig_action_flags[sig];
        oldact->sa_restorer = 0;
        oldact->sa_mask = current->sig_action_masks[sig];
    }
    if (act) {
        current->sig_handlers[sig] = act->sa_handler;
        current->sig_action_flags[sig] = (uint32_t)act->sa_flags;
        current->sig_action_masks[sig] = act->sa_mask;
        if (act->sa_handler == 1ULL) current->sig_pending &= ~(1ULL << sig);
    }
    return 0;
}

static int64_t riscv64_bootstrap_sys_getrandom(void* buf, size_t len, unsigned flags) {
    uint8_t* out = (uint8_t*)buf;
    uint64_t seed;
    (void)flags;
    if (!out) return -1;
    seed = arch_time_now_ms() ^ (uint64_t)(uintptr_t)get_current_task() ^ 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < len; i++) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        seed *= 0x2545F4914F6CDD1DULL;
        out[i] = (uint8_t)seed;
    }
    return (int64_t)len;
}

static int64_t riscv64_bootstrap_sys_waitid(int idtype, int id, struct riscv64_linux_siginfo* infop,
                                            int options) {
    int status = 0;
    int wait_pid;
    int target = -1;

    if (idtype == 0) target = id;
    else if (idtype == 1) target = -1;
    else return -1;

    wait_pid = (int)riscv64_bootstrap_sys_wait4(target, &status, options);
    if (wait_pid < 0) return wait_pid;
    if (infop) {
        for (size_t i = 0; i < sizeof(*infop); i++) ((uint8_t*)infop)[i] = 0;
        infop->si_signo = 17;
        infop->si_code = 1;
    }
    return 0;
}

static void* riscv64_bootstrap_sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    struct task* current = get_current_task();
    arch_address_space_t address_space;
    uint64_t base;
    uint64_t limit = 0x00007F0000000000ULL;
    uint64_t size;
    uint64_t map_flags;

    (void)addr;
    (void)offset;

    if (!current || length == 0) return (void*)-1;
    if ((flags & MAP_ANONYMOUS) == 0 || (flags & MAP_PRIVATE) == 0) return (void*)-1;
    if (fd != -1) return (void*)-1;

    size = riscv64_align_up_page((uint64_t)length);
    if (!size) return (void*)-1;

    base = current->mmap_end;
    if (base < RISCV64_USER_MMAP_BASE_VADDR) base = RISCV64_USER_MMAP_BASE_VADDR;
    base = riscv64_align_up_page(base);
    address_space = arch_task_context_get_address_space(&current->ctx);
    while (base + size <= limit) {
        uint64_t off = 0;
        int occupied = 0;
        while (off < size) {
            if (arch_vm_get_phys(address_space, base + off) != 0) {
                occupied = 1;
                break;
            }
            off += PAGE_SIZE;
        }
        if (!occupied) break;
        base += PAGE_SIZE;
    }
    if (base + size > limit) return (void*)-1;

    map_flags = arch_vm_user_page_flags((prot & PROT_WRITE) != 0, 0);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc(1);
        if (!phys) return (void*)-1;
        for (uint64_t i = 0; i < PAGE_SIZE; i++) {
            ((uint8_t*)(uintptr_t)phys)[i] = 0;
        }
        arch_vm_map_page(address_space, base + off, phys, map_flags);
    }
    current->mmap_end = base + size;
    riscv64_sfence_vma();
    return (void*)(uintptr_t)base;
}

static int64_t riscv64_bootstrap_sys_wait4(int pid, int* wstatus, int options) {
    struct task* current = get_current_task();
    (void)options;

    if (!current) return -1;

    while (1) {
        int found_child = 0;
        struct task* candidate = task_list;
        while (candidate) {
            if (candidate->ppid == current->pid && (pid == -1 || candidate->pid == pid)) {
                found_child = 1;
                if (candidate->state == TASK_ZOMBIE) {
                    int child_pid = candidate->pid;
                    if (wstatus) *wstatus = candidate->exit_status << 8;
                    (void)task_reap(candidate);
                    return child_pid;
                }
            }
            candidate = candidate->next;
        }
        if (!found_child) return -1;
        kernel_yield();
    }
}

static void riscv64_bootstrap_sys_exit(int status) {
    struct task* current = get_current_task();

    if (!current || current->ppid == 0) {
        (void)status;
        riscv64_uart_puts("  bootstrap user exit\n");
        riscv64_wait_forever();
    }

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd].in_use) {
            (void)sys_close(fd);
        }
    }
    (void)task_mark_zombie(current, status);
    while (1) kernel_yield();
}

static void riscv64_bootstrap_syscall_dispatch(arch_syscall_frame_t* frame) {
    uint64_t syscall_no;
    int64_t arg0_s;
    if (!frame) return;
    syscall_no = arch_syscall_number(frame);
    arg0_s = (int64_t)arch_syscall_arg0(frame);

    if (syscall_no == RISCV64_LINUX_SYS_NEWFSTATAT && arg0_s >= -4096 && arg0_s <= 4096) {
        int dirfd = (int)arch_syscall_arg0(frame);
        const char* path = (const char*)(uintptr_t)arch_syscall_arg1(frame);
        struct kstat* st = (struct kstat*)(uintptr_t)arch_syscall_arg2(frame);
        int flags = (int)arch_syscall_arg3(frame);
        int rc;
        if (path && path[0] == '\0' && (flags & RISCV64_LINUX_AT_EMPTY_PATH) != 0) {
            rc = sys_fstat(dirfd, st);
        } else {
            rc = sys_fstatat(dirfd, path, st, flags);
        }
        arch_syscall_set_return(frame, (uint64_t)(int64_t)rc);
        return;
    }

    if (syscall_no == RISCV64_LINUX_SYS_FSTAT && arg0_s >= -4096 && arg0_s <= 4096) {
        arch_syscall_set_return(frame,
                                (uint64_t)(int64_t)sys_fstat((int)arch_syscall_arg0(frame),
                                                             (struct kstat*)(uintptr_t)arch_syscall_arg1(frame)));
        return;
    }

    if (syscall_no == RISCV64_LINUX_SYS_GETDENTS64 &&
        arg0_s >= 0 && arg0_s < MAX_FDS &&
        arch_syscall_arg1(frame) >= 0x1000ULL &&
        arch_syscall_arg2(frame) >= sizeof(struct orth_dirent)) {
        arch_syscall_set_return(frame,
                                (uint64_t)(int64_t)sys_getdents64((int)arch_syscall_arg0(frame),
                                                                  (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                  (size_t)arch_syscall_arg2(frame)));
        return;
    }

    switch (syscall_no) {
        case SYS_WRITE:
        case RISCV64_LINUX_SYS_WRITE:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_write((int)arch_syscall_arg0(frame),
                                                                                    (const void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (size_t)arch_syscall_arg2(frame)));
            return;
        case SYS_GETCWD:
        case RISCV64_LINUX_SYS_GETCWD:
            {
                struct task* task = get_current_task();
                char* dst = (char*)(uintptr_t)arch_syscall_arg0(frame);
                size_t dst_size = (size_t)arch_syscall_arg1(frame);
                const char* cwd = (task && task->cwd[0]) ? task->cwd : "/";
                size_t i = 0;
                if (!dst || dst_size == 0) {
                    arch_syscall_set_return(frame, 0);
                    return;
                }
                while (cwd[i] && i + 1 < dst_size) {
                    dst[i] = cwd[i];
                    i++;
                }
                if (cwd[i] != '\0' && i + 1 >= dst_size) {
                    arch_syscall_set_return(frame, 0);
                    return;
                }
                dst[i] = '\0';
                arch_syscall_set_return(frame, (uint64_t)(uintptr_t)dst);
                return;
            }
        case SYS_GETPID:
        case RISCV64_LINUX_SYS_GETPID:
            {
                struct task* current = get_current_task();
                arch_syscall_set_return(frame, current ? (uint64_t)current->pid : 0);
                return;
            }
        case SYS_OPEN:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_open((const char*)(uintptr_t)arch_syscall_arg0(frame),
                                                                (int)arch_syscall_arg1(frame),
                                                                (int)arch_syscall_arg2(frame)));
            return;
        case SYS_OPENAT:
        case RISCV64_LINUX_SYS_OPENAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_openat((int)arch_syscall_arg0(frame),
                                                                  (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                  (int)arch_syscall_arg2(frame),
                                                                  (int)arch_syscall_arg3(frame)));
            return;
        case SYS_READ:
        case RISCV64_LINUX_SYS_READ:
            arch_syscall_set_return(frame,
                                    (uint64_t)sys_read((int)arch_syscall_arg0(frame),
                                                       (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                       (size_t)arch_syscall_arg2(frame)));
            return;
        case SYS_CLOSE:
        case RISCV64_LINUX_SYS_CLOSE:
            arch_syscall_set_return(frame, (uint64_t)(int64_t)sys_close((int)arch_syscall_arg0(frame)));
            return;
        case SYS_GETDENTS:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_getdents64((int)arch_syscall_arg0(frame),
                                                                      (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                      (size_t)arch_syscall_arg2(frame)));
            return;
        case SYS_FSTAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_fstat((int)arch_syscall_arg0(frame),
                                                                 (struct kstat*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case SYS_STAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_stat((const char*)(uintptr_t)arch_syscall_arg0(frame),
                                                                (struct kstat*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case SYS_FSTATAT:
            {
                int dirfd = (int)arch_syscall_arg0(frame);
                const char* path = (const char*)(uintptr_t)arch_syscall_arg1(frame);
                struct kstat* st = (struct kstat*)(uintptr_t)arch_syscall_arg2(frame);
                int flags = (int)arch_syscall_arg3(frame);
                int rc;
                if (path && path[0] == '\0' && (flags & RISCV64_LINUX_AT_EMPTY_PATH) != 0) {
                    rc = sys_fstat(dirfd, st);
                } else {
                    rc = sys_fstatat(dirfd, path, st, flags);
                }
                arch_syscall_set_return(frame, (uint64_t)(int64_t)rc);
            }
            return;
        case SYS_CHDIR:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_chdir((const char*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case SYS_FCHDIR:
            arch_syscall_set_return(frame, (uint64_t)(int64_t)sys_fchdir((int)arch_syscall_arg0(frame)));
            return;
        case SYS_MMAP:
        case RISCV64_LINUX_SYS_MMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(uintptr_t)riscv64_bootstrap_sys_mmap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                    (size_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame),
                                                                                    (int)arch_syscall_arg3(frame),
                                                                                    (int)arch_syscall_arg4(frame),
                                                                                    (int64_t)arch_syscall_arg5(frame)));
            return;
        case SYS_MUNMAP:
        case RISCV64_LINUX_SYS_MUNMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_munmap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                     (size_t)arch_syscall_arg1(frame)));
            return;
        case SYS_BRK:
        case RISCV64_LINUX_SYS_BRK:
            arch_syscall_set_return(frame, riscv64_bootstrap_sys_brk(arch_syscall_arg0(frame)));
            return;
        case SYS_WAIT4:
        case RISCV64_LINUX_SYS_WAIT4:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_wait4((int)arch_syscall_arg0(frame),
                                                                                    (int*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case RISCV64_LINUX_SYS_WAITID:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_waitid((int)arch_syscall_arg0(frame),
                                                                                    (int)arch_syscall_arg1(frame),
                                                                                    (struct riscv64_linux_siginfo*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                    (int)arch_syscall_arg3(frame)));
            return;
        case RISCV64_LINUX_SYS_SET_TID_ADDRESS:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_set_tid_address((int*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case RISCV64_LINUX_SYS_FUTEX:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_futex((volatile int*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                    (int)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case RISCV64_LINUX_SYS_CLOCK_GETTIME:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_clock_gettime((int)arch_syscall_arg0(frame),
                                                                                            (struct riscv64_linux_timespec*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case RISCV64_LINUX_SYS_RT_SIGACTION:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_rt_sigaction((int)arch_syscall_arg0(frame),
                                                                                           (const struct riscv64_linux_sigaction*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                           (struct riscv64_linux_sigaction*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                           (size_t)arch_syscall_arg3(frame)));
            return;
        case RISCV64_LINUX_SYS_RT_SIGPROCMASK:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_rt_sigprocmask((int)arch_syscall_arg0(frame),
                                                                                             (const uint64_t*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                             (uint64_t*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                             (size_t)arch_syscall_arg3(frame)));
            return;
        case RISCV64_LINUX_SYS_GETRANDOM:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_getrandom((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                        (size_t)arch_syscall_arg1(frame),
                                                                                        (unsigned)arch_syscall_arg2(frame)));
            return;
        case RISCV64_LINUX_SYS_WRITEV:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_writev((int)arch_syscall_arg0(frame),
                                                                                    (const struct riscv64_linux_iovec*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case RISCV64_LINUX_SYS_READV:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_readv((int)arch_syscall_arg0(frame),
                                                                                   (const struct riscv64_linux_iovec*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                   (int)arch_syscall_arg2(frame)));
            return;
        case RISCV64_LINUX_SYS_LSEEK:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_lseek((int)arch_syscall_arg0(frame),
                                                                                   (int64_t)arch_syscall_arg1(frame),
                                                                                   (int)arch_syscall_arg2(frame)));
            return;
        case RISCV64_LINUX_SYS_IOCTL:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)riscv64_bootstrap_sys_ioctl((int)arch_syscall_arg0(frame),
                                                                                   (unsigned long)arch_syscall_arg1(frame),
                                                                                   arch_syscall_arg2(frame)));
            return;
        case SYS_EXIT:
        case RISCV64_LINUX_SYS_EXIT:
        case SYS_EXIT_GROUP:
        case RISCV64_LINUX_SYS_EXIT_GROUP:
            riscv64_bootstrap_sys_exit((int)arch_syscall_arg0(frame));
            return;
        default:
            arch_syscall_set_return(frame, (uint64_t)-38);
            return;
    }
}

void riscv64_syscall_dispatch(riscv64_trap_frame_t* frame) {
    arch_syscall_frame_t syscall_frame;
    if (!frame) return;

    if ((frame->a7 == SYS_FORK && frame->a0 == 0 && frame->a1 == 0 && frame->a2 == 0) ||
        (frame->a7 == RISCV64_LINUX_SYS_CLONE && frame->a0 == 17 && frame->a1 == 0 && frame->a2 == 0)) {
        int ret = task_fork(frame);
        riscv64_trap_set_user_return(frame, frame->sepc + 4, frame->sp, (uint64_t)(int64_t)ret, frame->a1, frame->a2);
        riscv64_syscall_sync_current_user_frame(frame);
        return;
    }

    syscall_frame.r15 = 0;
    syscall_frame.r14 = 0;
    syscall_frame.r13 = 0;
    syscall_frame.r12 = 0;
    syscall_frame.rbp = frame->s0;
    syscall_frame.rbx = frame->s1;
    arch_syscall_set_arg5(&syscall_frame, frame->a5);
    arch_syscall_set_arg4(&syscall_frame, frame->a4);
    arch_syscall_set_arg3(&syscall_frame, frame->a3);
    arch_syscall_set_arg2(&syscall_frame, frame->a2);
    arch_syscall_set_arg1(&syscall_frame, frame->a1);
    arch_syscall_set_arg0(&syscall_frame, frame->a0);
    arch_syscall_set_number(&syscall_frame, frame->a7);
    arch_syscall_set_program_counter(&syscall_frame, frame->sepc + 4);
    syscall_frame.cs = 0;
    syscall_frame.rflags = frame->sstatus;
    arch_syscall_set_stack_pointer(&syscall_frame, frame->sp);
    syscall_frame.ss = 0;

    riscv64_bootstrap_syscall_dispatch(&syscall_frame);

    frame->a3 = syscall_frame.r10;
    frame->a4 = syscall_frame.r8;
    frame->a5 = syscall_frame.r9;
    frame->s0 = syscall_frame.rbp;
    frame->s1 = syscall_frame.rbx;
    riscv64_trap_set_user_return(frame,
                                 arch_syscall_program_counter(&syscall_frame),
                                 arch_syscall_stack_pointer(&syscall_frame),
                                 arch_syscall_return(&syscall_frame),
                                 arch_syscall_arg1(&syscall_frame),
                                 arch_syscall_arg2(&syscall_frame));
    riscv64_syscall_sync_current_user_frame(frame);
}

void riscv64_syscall_sync_current_user_frame(const riscv64_trap_frame_t* frame) {
    task_context_t* ctx;
    if (!frame) return;
    ctx = task_current_context();
    if (!ctx) ctx = g_riscv64_fallback_current_context;
    if (!ctx) return;
    riscv64_task_store_user_frame(ctx, frame);
}

void riscv64_syscall_set_current_context(struct arch_task_context* ctx) {
    g_riscv64_fallback_current_context = (task_context_t*)ctx;
}
