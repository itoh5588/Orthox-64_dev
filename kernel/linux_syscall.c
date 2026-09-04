#include <stdint.h>
#include "fs.h"
#include "stdio.h"
#include "pmm.h"
#include "vmm.h"
#include "linux_errno.h"
#include "spinlock.h"
#include "linux_syscalls.h"
#include "arch_syscall.h"
#include "arch_vm.h"   /* mprotect が権限ビットを組み立てる */
#include "string.h"
#include "linux_syscall.h"
#include "syscall.h"
#include "sys_internal.h"
#include "task.h"
#include "xv6fs.h"   /* xv6fs_now_sec。CLOCK_REALTIME をファイルの時計と揃える */
#include "wait.h"    /* wait4 を焼かずに寝かせる */

static task_context_t* g_linux_fallback_current_context;
extern int task_fork(arch_task_exec_frame_t* frame);
extern int task_execve(arch_task_exec_frame_t* frame, const char* path,
                       char* const argv[], char* const envp[]);
extern struct task* task_list;
static int64_t linux_bootstrap_sys_wait4(int pid, int* wstatus, int options);

/* Linux/RISC-V の SIGCHLD。wait4 で眠った親を起こすためにも使う。 */
#define LINUX_SIGCHLD 17
#define LINUX_SIGINT  2
#define LINUX_SIGQUIT 3

static struct task* linux_find_task_by_pid(int pid) {
    struct task* task = task_list;
    while (task) {
        if (task->pid == pid) return task;
        task = task->next;
    }
    return 0;
}

/* wait4 で待つ親を寝かせる待ち行列。
 *
 * **以前は kernel_yield() で回し続けていた。**待っている親はコアを 1 本
 * 100% 焼く。ash と configure の sh が同時に待つと、4 コアのうち 2 本が
 * 待つためだけに消える —— 日報2026-08-29 の G-2「configure はほぼ逐次の
 * はずなのに 2 コアが 100%」の正体がこれだった (2026-08-30、実機の
 * [pc] 計器が 2 本とも linux_bootstrap_sys_wait4 を指した)。
 *
 * **時間切れつきで寝る。**task_mark_zombie は 5 か所から呼ばれるのに、
 * 親を起こす linux_notify_parent_exit は exit の経路にしか無い
 * (Ctrl-C / SIGSEGV / kill は通らない)。素直に寝かせると、それらの経路で
 * 親が永久に起きず、**空回りより悪くなる。**上限を置けば、起こし損ねても
 * 「その回だけ遅い」で済む。普段は wake_up_all で即座に起きる。
 *
 * wait_queue は spinlock_init と head=0 しかしないので、静的変数は
 * ゼロ初期化のまま使える (kernel/wait.c の wait_queue_init 参照)。 */
#define LINUX_WAIT4_POLL_MS 50
static struct wait_queue g_child_exit_wq;

/* WNOHANG。**以前は options を捨てていた**ので、子がまだ終わっていない
 * ときに 0 を返さず待ち続けていた */
#define LINUX_WNOHANG 1

struct linux_child_wait { int ppid; int want; };

static int linux_child_zombie_ready(void* arg) {
    struct linux_child_wait* w = (struct linux_child_wait*)arg;
    struct task* t = task_list;
    while (t) {
        if (t->ppid == w->ppid && (w->want == -1 || t->pid == w->want) &&
            t->state == TASK_ZOMBIE) {
            return 1;
        }
        t = t->next;
    }
    return 0;
}

/*
 * 子の終了は、zombie 化だけでは待機中の親へ伝わらない。特に BusyBox ash の
 * $(cmd | cmd) はブロッキング waitpid に入り得るため、親を起こさないと
 * TASK_SLEEPING のまま残る。pipe の close より先にこの通知を行う必要はなく、
 * 親が wait4 を再走査した時点で child の zombie 状態が見えていればよい。
 */
static void linux_notify_parent_exit(struct task* child) {
    struct task* parent;
    if (!child || child->ppid <= 0) return;
    /* **待ち行列は親を特定せずに全部起こす。**起こされた側は述語で
     * 「自分の子か」を見るので、無関係な親は寝直すだけ */
    wake_up_all(&g_child_exit_wq);
    parent = linux_find_task_by_pid(child->ppid);
    if (!parent) return;
    parent->sig_pending |= (1ULL << LINUX_SIGCHLD);
    if (parent->state == TASK_SLEEPING) (void)task_wake(parent);
}

#define LINUX_USER_MMAP_BASE_VADDR 0x0000002000000000ULL

/* 未実装の syscall を ENOSYS で返すとき、番号を 1 回だけ出す。
 * 黙って失敗値を返すと、呼び出し側が戻り値を見ていない場合に
 * 「成功したのに何も起きない」形になり、原因の特定が極端に難しくなる。
 * 番号ごとに 1 回だけなので、ループで呼ばれてもログは溢れない。 */
#define LINUX_ENOSYS_SEEN_MAX 64
static uint64_t g_linux_enosys_seen[LINUX_ENOSYS_SEEN_MAX];
static int g_linux_enosys_seen_count;

static void linux_report_unimplemented_syscall(uint64_t number) {
    for (int i = 0; i < g_linux_enosys_seen_count; i++) {
        if (g_linux_enosys_seen[i] == number) return;
    }
    if (g_linux_enosys_seen_count < LINUX_ENOSYS_SEEN_MAX) {
        g_linux_enosys_seen[g_linux_enosys_seen_count++] = number;
    }
    puts("ENOSYS: syscall ");
    puthex(number);
    puts("\n");
}

/* ファイル系 syscall の一時トレース。既定では無効で、
 *   make riscv64-kernel RISCV64_EXTRA_CFLAGS=-DLINUX_SYSCALL_TRACE=1
 * のときだけ入る。ユーザー空間から見た「成功したのに書けていない」を
 * 追うためのもので、常用のカーネルには入れない。 */
#ifdef LINUX_SYSCALL_TRACE
static uint64_t g_linux_trace_num;
static uint64_t g_linux_trace_a0;
static uint64_t g_linux_trace_a1;
static uint64_t g_linux_trace_a2;
static int g_linux_trace_active;

static int linux_syscall_trace_wanted(uint64_t number) {
    switch (number) {
        case 23:  /* dup */
        case 24:  /* dup3 */
        case 25:  /* fcntl */
        case 35:  /* unlinkat */
        case 63:  /* read */
        case 45:  /* truncate */
        case 46:  /* ftruncate */
        case 56:  /* openat */
        case 57:  /* close */
        case 62:  /* lseek */
        case 64:  /* write */
        case 66:  /* writev */
        case 68:  /* pwrite64 */
        case 82:  /* fsync */
        case 38:  /* renameat */
        case 276: /* renameat2 */
            return 1;
        default:
            return 0;
    }
}

static void linux_trace_puts_user(const char* s) {
    if (!s) {
        puts("(null)");
        return;
    }
    for (int i = 0; i < 96 && s[i]; i++) {
        char buf[2] = {s[i], 0};
        puts(buf);
    }
}

static void linux_syscall_trace_enter(const arch_syscall_frame_t* frame) {
    g_linux_trace_active = linux_syscall_trace_wanted(arch_syscall_number(frame));
    if (!g_linux_trace_active) return;
    g_linux_trace_num = arch_syscall_number(frame);
    g_linux_trace_a0 = arch_syscall_arg0(frame);
    g_linux_trace_a1 = arch_syscall_arg1(frame);
    g_linux_trace_a2 = arch_syscall_arg2(frame);
}

static void linux_syscall_trace_leave(const arch_syscall_frame_t* frame) {
    if (!g_linux_trace_active) return;
    g_linux_trace_active = 0;
    puts("TR ");
    puthex(g_linux_trace_num);
    puts(" a0=");
    puthex(g_linux_trace_a0);
    puts(" a1=");
    puthex(g_linux_trace_a1);
    puts(" a2=");
    puthex(g_linux_trace_a2);
    puts(" -> ");
    puthex(arch_syscall_arg0(frame));
    if (g_linux_trace_num == 56 || g_linux_trace_num == 35) {
        /* openat(dirfd, path, ...) / unlinkat(dirfd, path, ...) */
        puts(" path=");
        linux_trace_puts_user((const char*)(uintptr_t)g_linux_trace_a1);
    }
    if (g_linux_trace_num == 45) {
        /* truncate(path, len) は a0 が path */
        puts(" path=");
        linux_trace_puts_user((const char*)(uintptr_t)g_linux_trace_a0);
    }
    if (g_linux_trace_num == 276) {
        puts(" from=");
        linux_trace_puts_user((const char*)(uintptr_t)g_linux_trace_a1);
    }
    puts("\n");
}
#else
static void linux_syscall_trace_enter(const arch_syscall_frame_t* frame) { (void)frame; }
static void linux_syscall_trace_leave(const arch_syscall_frame_t* frame) { (void)frame; }
#endif

/* clone(2) のフラグ (musl の vfork が使う分だけ) */
#define LINUX_CLONE_VM      0x00000100ULL
#define LINUX_CLONE_VFORK   0x00004000ULL
#define LINUX_CLONE_SIGCHLD 17ULL

#define LINUX_TCGETS               0x5401UL
#define LINUX_TCSETS               0x5402UL
#define LINUX_TIOCGPGRP            0x540FUL
#define LINUX_TIOCSPGRP            0x5410UL
#define LINUX_TIOCGWINSZ           0x5413UL

#define LINUX_SIG_BLOCK   0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct linux_iovec {
    void* iov_base;
    size_t iov_len;
};

struct linux_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

struct linux_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    uint8_t payload[112];
};

struct linux_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

// riscv64 (asm-generic) の struct stat。x86_64 レイアウトの struct kstat とは
// フィールド順が異なるため、ユーザーへ返す際に変換する
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t __pad2;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
};

static void linux_stat_from_kstat(struct linux_stat* out, const struct kstat* st) {
    if (!out || !st) return;
    out->st_dev = st->dev;
    out->st_ino = st->ino;
    out->st_mode = st->mode;
    out->st_nlink = (uint32_t)st->nlink;
    out->st_uid = st->uid;
    out->st_gid = st->gid;
    out->st_rdev = st->rdev;
    out->__pad1 = 0;
    out->st_size = st->size;
    out->st_blksize = 512;
    out->__pad2 = 0;
    out->st_blocks = (st->size + 511) / 512;
    out->st_atime_sec = st->atime_sec;
    out->st_atime_nsec = 0;
    out->st_mtime_sec = st->mtime_sec;
    out->st_mtime_nsec = 0;
    out->st_ctime_sec = st->ctime_sec;
    out->st_ctime_nsec = 0;
    out->__unused4 = 0;
    out->__unused5 = 0;
}

static int linux_sys_fstat_user(int fd, struct linux_stat* user_st) {
    struct kstat st;
    int rc = sys_fstat(fd, &st);
    if (rc == 0 && user_st) linux_stat_from_kstat(user_st, &st);
    return rc;
}

static int linux_sys_fstatat_user(int dirfd, const char* path, struct linux_stat* user_st, int flags) {
    struct kstat st;
    int rc = sys_fstatat(dirfd, path, &st, flags);
    if (rc == 0 && user_st) linux_stat_from_kstat(user_st, &st);
    return rc;
}

/* Linux の struct utsname (asm-generic は各フィールド 65 バイト固定) */
struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static void linux_utsname_set(char* dst, const char* src) {
    size_t i = 0;
    for (; src[i] && i < 64; i++) dst[i] = src[i];
    for (; i < 65; i++) dst[i] = '\0';
}

static int linux_sys_uname(struct linux_utsname* out) {
    if (!out) return -LINUX_EFAULT;
    linux_utsname_set(out->sysname, "Linux");
    linux_utsname_set(out->nodename, "orthox");
    /* busybox の一部は release を Linux のバージョンとしてパースするので
     * それらしい形にしておく。実体は Orthox-64 なので version 側で名乗る */
    linux_utsname_set(out->release, "5.0.0-orthox");
    linux_utsname_set(out->version, arch_uname_version());
    linux_utsname_set(out->machine, arch_uname_machine());
    linux_utsname_set(out->domainname, "(none)");
    return 0;
}

/* ---- 資源の上限 (getrlimit / setrlimit / prlimit64) ----------------------
 *
 * **実測で ENOSYS が出たので埋めた (2026-08-11)。** Orthox の中で gcc / ld を
 * 動かすと呼ばれる。Orthox には資源制限の仕組みが無いので、**答えるだけで
 * 記憶しない**。設定側 (setrlimit / prlimit64 の new) は黙って受ける —
 * ここで EPERM を返すと、上限を下げようとする側が異常終了することがある。
 *
 * **無限で答えてよいものと、実際の値を答えるべきものがある。**
 *   NOFILE  MAX_FDS を答える。無限にすると、呼び手が上限まで
 *           close() を回して大量に空振りする
 *   STACK   実際に張っている大きさ。無限だと alloca を使う側が踏み外す */
static void linux_rlimit_for(int resource, struct linux_rlimit64* out) {
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

static int linux_sys_getrlimit(int resource, struct linux_rlimit64* rlim) {
    if (resource < 0 || resource >= LINUX_RLIMIT_NLIMITS) return -LINUX_EINVAL;
    if (!rlim) return -LINUX_EFAULT;
    linux_rlimit_for(resource, rlim);
    return 0;
}

/* 上限は記憶しないが、**成功を返す**。失敗にすると呼び手が落ちる */
static int linux_sys_setrlimit(int resource, const struct linux_rlimit64* rlim) {
    if (resource < 0 || resource >= LINUX_RLIMIT_NLIMITS) return -LINUX_EINVAL;
    if (!rlim) return -LINUX_EFAULT;
    return 0;
}

static int linux_sys_prlimit64(int pid, int resource,
                               const struct linux_rlimit64* new_limit,
                               struct linux_rlimit64* old_limit) {
    struct task* current = get_current_task();
    if (resource < 0 || resource >= LINUX_RLIMIT_NLIMITS) return -LINUX_EINVAL;
    /* pid 0 は自分。他プロセスは見ない */
    if (pid != 0 && current && pid != current->pid) return -LINUX_EPERM;
    if (old_limit) linux_rlimit_for(resource, old_limit);
    (void)new_limit;   /* 記憶しない (上のコメント) */
    return 0;
}

/* **中身は 0 でよい。** 呼び手は「取れたかどうか」しか見ない。
 * 0 を返さずに ENOSYS にすると、gcc が時間計測を諦めずに落ちることがある */
static int linux_sys_getrusage(int who, struct linux_rusage* usage) {
    uint8_t* p;
    if (who != LINUX_RUSAGE_SELF && who != LINUX_RUSAGE_CHILDREN) return -LINUX_EINVAL;
    if (!usage) return -LINUX_EFAULT;
    p = (uint8_t*)usage;
    for (uint64_t i = 0; i < sizeof(*usage); i++) p[i] = 0;
    return 0;
}

/* times(2)。**返り値だけは本物を返す。**GCC の timevar.c は
 *
 *     now->wall = times (&tms) * ticks_to_msec;
 *
 * と返り値をそのまま壁時計に使うので、ENOSYS で -1 が返ると
 * -ftime-report の数字が全部でたらめになる。2026-08-30 に Pi 4 実機で
 * セルフホストした cc1 が
 *
 *     phase setup : -0.80 ( 0%) usr1316.20 (2632400%) sys
 *
 * のような表示を出した。**中身を 0 にするだけでは足りない**のがここ。
 *
 * 単位は CLK_TCK で、musl の aarch64 は 100 固定なので 10ms を 1 とする。
 * プロセスごとの CPU 時間は記録していないので tms の 4 つは 0 でよい。
 * user / sys は 0.00 と出るが、wall は正しくなる。
 * buf が NULL でも呼べる (Linux も許す) */
struct linux_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

static int64_t linux_sys_times(struct linux_tms* buf) {
    if (buf) {
        buf->tms_utime  = 0;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return (int64_t)(arch_time_now_ms() / 10ULL);
}

/* fchown(2) / fchownat(2)。**所有者を変える要求は常に成功でよい。**
 * xv6fs の inode に uid / gid の欄が無く、Orthox は uid 0 の単一利用者で
 * 動くので、「root のものにせよ」は既に満たされている。ENOSYS を返して
 * いたときは GCC のビルドで cp -p が
 *
 *     cp: can't preserve ownership of 'include-fixed/limits.h'
 *
 * を出していた (2026-08-30 に実機で確認)。**黙って 0 を返すのではなく、
 * 相手が本当に在るかだけは見る。**無いものに成功を返すと、呼び手が
 * 「作れた」と思い込んで次で転ぶ */
static int linux_sys_fchown(int fd) {
    struct kstat st;
    return sys_fstat(fd, &st);
}

static int linux_sys_fchownat(int dirfd, const char* path, int flags) {
    struct kstat st;
    if (path && path[0] == '\0' && (flags & LINUX_AT_EMPTY_PATH) != 0)
        return sys_fstat(dirfd, &st);
    return sys_fstatat(dirfd, path, &st, flags);
}

/* umask はプロセスごとの状態で fork で引き継ぐ (include/task.h)。
 * **古い値を返すのが仕様。** 返さないと、保存して戻す側が壊れる */
static int linux_sys_umask(int mask) {
    struct task* current = get_current_task();
    int old;
    if (!current) return -LINUX_ESRCH;
    old = (int)current->umask;
    current->umask = (uint32_t)mask & 0777U;
    return old;
}

static int linux_sys_sysinfo(struct linux_sysinfo* info) {
    uint8_t* p;
    if (!info) return -LINUX_EFAULT;
    p = (uint8_t*)info;
    for (uint64_t i = 0; i < sizeof(*info); i++) p[i] = 0;
    /* **mem_unit を 0 にしないこと。** 呼び手が totalram に掛けるので
     * 0 だと「メモリ 0」に見え、確保をあきらめる側がいる */
    info->mem_unit = PAGE_SIZE;
    info->totalram = pmm_get_allocated_pages() + pmm_get_free_pages();
    info->freeram  = pmm_get_free_pages();
    info->procs    = 1;
    return 0;
}

/* N-6 (2026-08-31): xv6fs に本物のシンボリックリンクを実装したので、
 * fs_readlinkat にそのまま委ねる。戻り値は fs.c 側の errno 定数と
 * Linux の値が同じ番号を使っているのでそのまま渡してよい (どちらも
 * ENOENT=2 / EINVAL=22 など、fs.c 冒頭の #define がその前提で書かれている) */
static int64_t linux_sys_readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz) {
    if (!path || !buf) return -LINUX_EFAULT;
    return fs_readlinkat(dirfd, path, buf, bufsiz);
}

struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static int g_linux_tty_pgrp;
static struct linux_termios g_linux_console_termios = {
    .c_iflag = 0x00000002u,
    .c_oflag = 0x00000001u,
    .c_cflag = 0,
    .c_lflag = 0x00000001u | 0x00000002u | 0x00000008u,
    .c_cc = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26 },
    .c_ispeed = 115200,
    .c_ospeed = 115200,
};

/* termios の ECHO (c_lflag bit3)。行編集 (busybox の lineedit) は raw モードに
 * して自前でエコーするので、ここが立っていないときにカーネルがエコーすると
 * 1 文字が 2 回出る。fs.c のコンソール読み取りが参照する */
int arch_console_echo_enabled(void) {
    return (g_linux_console_termios.c_lflag & 0x00000008u) != 0;
}

/* ---- M-9 (2026-08-31): シリアル/USB キーボードの ^C / ^\ ----------------
 *
 * termios は ISIG (c_lflag bit0) が立ち、VINTR=3 (^C) / VQUIT=28 (^\) も
 * 最初から設定されていたのに、割り込み文字を見て実際に配送する経路が
 * どこにも無かった。**リングに積む前に判定できるよう、ロックなしで
 * 呼べる判定関数と、フォアグラウンドプロセスグループを落とす関数を
 * 分けた。**後者は task_mark_zombie が別のロックを取るので、呼び出し側
 * (aarch64/console.c) はコンソールの割り込みロックを持ったまま呼ばないこと
 * (task_wake と同じ扱い)。 */
int linux_console_is_intr_char(uint8_t ch) {
    if ((g_linux_console_termios.c_lflag & 0x00000001u) == 0) return 0;   /* ISIG off */
    return ch == (uint8_t)g_linux_console_termios.c_cc[0] ||   /* VINTR */
           ch == (uint8_t)g_linux_console_termios.c_cc[1];     /* VQUIT */
}

/* M-8 (2026-08-31): pid の子を pid 1 に引き取らせる。**本来の init のような
 * 「拾って wait する」回収ループが無いので、既に zombie な子は引き取っても
 * 誰も reap しない。**親が消える前にここで reap してしまう。まだ走っている
 * 子は ppid を付け替えるだけにする —— 死んだ pid を指したままにはしない。
 *
 * task_reap は task_list からノードを外して解放するので、**呼び出し側が
 * 同じ巡回の中でこれを呼ぶときは、先に t->next を控えてから呼ぶこと。** */
static void linux_reap_orphans_of(int pid) {
    struct task* t = task_list;
    while (t) {
        struct task* next = t->next;
        if (t->ppid == pid) {
            if (t->state == TASK_ZOMBIE) {
                (void)task_reap(t);
            } else {
                t->ppid = 1;
            }
        }
        t = next;
    }
}

/* x86 (kernel/keyboard.c の send_sigint_to_foreground_pgrp) と同じ、
 * 「本物のシグナル配送ではなく即座に zombie にする」簡易実装。
 * job control (setpgid) が無いので、フォアグラウンドの pgid は
 * ash 自身の pid と揃っていることが多い。pid 1 (シェル自身) は
 * 除外して、それ以外の同じ pgid のタスクだけを落とす。
 *
 * **2 巡に分けている。**1 巡目で zombie にする間は task_list のリンクは
 * 変わらない (task_mark_zombie はリストを触らない) ので安全に辿れるが、
 * 2 巡目の task_reap はノードを外すので、1 巡目の途中で呼ぶと次のノードを
 * 見失う。落とした pid を控えておいて、1 巡目が終わってから 2 巡目で
 * それぞれの子を片付ける。 */
void linux_console_deliver_intr(uint8_t ch) {
    struct task* t = task_list;
    int fg = g_linux_tty_pgrp;
    int sig = (ch == (uint8_t)g_linux_console_termios.c_cc[1]) ? LINUX_SIGQUIT : LINUX_SIGINT;
    int victim_pids[64];
    int nvictims = 0;
    if (fg == 0) return;   /* まだ誰も TIOCSPGRP/TIOCGPGRP していない */

    while (t) {
        if (t->pgid == fg && t->pid != 1) {
            t->sig_pending |= (1ULL << sig);
            task_mark_zombie(t, 128 + sig);
            if (nvictims < 64) victim_pids[nvictims++] = t->pid;
        }
        t = t->next;
    }

    for (int i = 0; i < nvictims; i++) {
        linux_reap_orphans_of(victim_pids[i]);
    }
}

/* termios の ONLCR (c_oflag bit0)。**LF だけでは実機の端末は行頭に戻らない。**
 *
 * QEMU の -serial stdio ではホスト端末が LF を改行として扱うので見えないが、
 * 実機のシリアル端末 (Tera Term) では次の行が前の行の終端位置から始まる。
 * Pi 4 実機の ash で `ls` の段組みが階段状に崩れて発覚した。
 *
 * カーネルの起動ログ (aarch64_uart_puts) は自前で CR を出していたので、
 * **ユーザープロセスの write だけが崩れていた**。 */
int arch_console_onlcr_enabled(void) {
    return (g_linux_console_termios.c_oflag & 0x00000001u) != 0;
}

static uint64_t linux_align_up_page(uint64_t value) {
    return (value + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
}

static int64_t linux_bootstrap_sys_write(int fd, const void* buf, size_t count) {
    return sys_write(fd, buf, count);
}

static int64_t linux_bootstrap_sys_lseek(int fd, int64_t offset, int whence) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    int64_t base;
    int64_t next;

    /* 戻り値は -errno 規約。-1 は EPERM として顕在化するので使わない */
    if (!current) return -LINUX_ESRCH;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -LINUX_EBADF;
    f = &current->fds[fd];
    if (f->type == FT_DIR) return -LINUX_ESPIPE;
    /* offset / size は共有 open file description (fd->file) 側にある。
     * dup / fork した相方の書き込みがここに見えるのはそのため。
     * 別々に open した fd は別の file を持つので、xv6fs は inode から
     * 取り直す (arch_fs_refresh_size) */
    arch_fs_refresh_size(f);

    switch (whence) {
        case 0:
            base = 0;
            break;
        case 1:
            base = (int64_t)fs_fd_offset(f);
            break;
        case 2:
            base = (int64_t)fs_fd_size(f);
            break;
        default:
            return -LINUX_EINVAL;
    }

    next = base + offset;
    if (next < 0) return -LINUX_EINVAL;
    /* **書き込み用に開いたファイルは EOF 越えのシークを許す (穴あき書き込み)。**
     *
     * **ramfs を外していて壊れた (P-9、2026-08-29)。** /tmp を ramfs に置いた
     * 途端に `gcc -static` が落ちた:
     *
     *   as: can't write 56 bytes to section .text of /tmp/ccYYYY.o:
     *       'file truncated'   (BFD assertion fail bfd/elf.c:3663)
     *
     * as はセクションを置くために EOF より先へ seek してから書く。ここで
     * EINVAL を返すと **offset が動かないまま write が通る**ので、中身が
     * 別の場所に落ちて、呼び出し側からは「短いファイル」に見える。
     * **失敗の形が seek ではなく write 側に出るので分かりにくい。**
     *
     * 最小再現 (日報2026-08-29 §24):
     *   lseek(fd,3000,SEEK_SET) が /tmp では -1、xv6fs 上では 3000
     *
     * ramfs の書き込みは ramfs_grow(off + count) で伸ばし、**新しい領域を
     * 0 で埋める**ので、穴は正しく 0 として読める。 */
    if ((uint64_t)next > fs_fd_size(f) &&
        !((f->type == FT_XV6FS || f->type == FT_RAMFS) &&
          ((f->flags & 3) == O_WRONLY || (f->flags & 3) == O_RDWR))) {
        return -LINUX_EINVAL;
    }
    fs_fd_set_offset(f, (size_t)next);
    return next;
}

static int linux_bootstrap_sys_fchmodat(int dirfd, const char* path, uint32_t mode) {
    if (!path) return -LINUX_EFAULT;
    if (path[0] == '\0') return -LINUX_ENOENT;
    /* AT_FDCWD(-100) と絶対パスのみ対応。sys_chmod が cwd 相対を解決する。
     * それ以外の dirfd は解決できないので EBADF を返す (「この dirfd は使えない」) */
    if (path[0] != '/' && dirfd != -100) return -LINUX_EBADF;
    return sys_chmod(path, mode);
}

static int linux_bootstrap_sys_fchmod(int fd, uint32_t mode) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return -LINUX_EBADF;
    /* パスを持たない fd (パイプ / 疑似デバイス) は xv6fs の inode を辿れない */
    if (current->fds[fd].name[0] == '\0') return -LINUX_EINVAL;
    return sys_chmod(current->fds[fd].name, mode);
}

static int64_t linux_bootstrap_sys_writev(int fd, const struct linux_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (iovcnt < 0) return -LINUX_EINVAL;
    if (iovcnt != 0 && !iov) return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = linux_bootstrap_sys_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

static int64_t linux_bootstrap_sys_readv(int fd, const struct linux_iovec* iov, int iovcnt) {
    int64_t total = 0;
    if (iovcnt < 0) return -LINUX_EINVAL;
    if (iovcnt != 0 && !iov) return -LINUX_EFAULT;
    for (int i = 0; i < iovcnt; i++) {
        int64_t rc = sys_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return (total > 0) ? total : rc;
        total += rc;
        if ((size_t)rc != iov[i].iov_len) break;
    }
    return total;
}

/* ユーザーに配るページを 1 枚取って 0 で埋め、**物理アドレス**を返す。
 * 取れなければ 0。
 *
 * **pmm_alloc が返すのは物理アドレスなので、そのまま触ってはいけない。**
 * ここは元々 kernel/riscv64/syscall.c に居たコードで、riscv64 は
 * g_hhdm_offset が 0 (恒等) なため直書きでも動いていた。aarch64 は
 * 恒等マッピングを外して上位 VA だけで走るので、載せた瞬間に落ちる:
 *
 *   ESR=0x96000045  EC=0x25 (EL1 のデータアボート)
 *                   DFSC=0x05 translation fault level 1, WnR=1
 *   FAR=0x000000004036c000  (= 物理アドレスそのもの)
 *
 * **オフセット 0 を暗黙に前提にした共有コードだった。** brk と mmap が
 * 同じ書き方をしていたので 1 か所にまとめる */
static uint64_t linux_alloc_zeroed_user_page(void) {
    uint64_t phys = (uint64_t)(uintptr_t)pmm_alloc(1);
    uint8_t* mapped;
    if (!phys) return 0;
    mapped = (uint8_t*)PHYS_TO_VIRT(phys);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) mapped[i] = 0;
    return phys;
}

static uint64_t linux_bootstrap_sys_brk(uint64_t addr) {
    struct task* current = get_current_task();
    uint64_t current_page;
    uint64_t target_page;
    arch_address_space_t address_space;

    if (!current) return 0;
    if (addr == 0 || addr <= current->heap_break) return current->heap_break;

    current_page = linux_align_up_page(current->heap_break);
    target_page = linux_align_up_page(addr);
    address_space = arch_task_context_get_address_space(&current->ctx);

    while (current_page < target_page) {
        uint64_t phys = linux_alloc_zeroed_user_page();
        if (!phys) return current->heap_break;
        arch_vm_map_page(address_space, current_page, phys, arch_vm_user_page_flags(1, 0));
        current_page += PAGE_SIZE;
    }

    current->heap_break = addr;
    arch_syscall_flush_tlb();
    return current->heap_break;
}

static int linux_bootstrap_sys_munmap(void* addr, size_t length) {
    struct task* current = get_current_task();
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t size = linux_align_up_page((uint64_t)length);
    arch_address_space_t address_space;

    if (!current) return -LINUX_ESRCH;
    /* Linux の munmap は addr がページ境界でない / length が 0 を EINVAL とする */
    if (!addr || length == 0) return -LINUX_EINVAL;
    if ((base & (PAGE_SIZE - 1ULL)) != 0) return -LINUX_EINVAL;

    address_space = arch_task_context_get_address_space(&current->ctx);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        /* **外す前に物理ページを取って返すこと。**
         *
         * arch_vm_unmap_page は名前のとおり「写像を外す」だけで、物理ページの
         * 持ち主を変えない。外しただけだと **そのページは誰の物でもなくなり、
         * 参照カウントが 1 のまま二度と配られない**。
         *
         * musl の malloc は大きい塊を mmap / munmap で扱うので、cc1 のように
         * 確保と解放を繰り返すプログラムで積み上がる。実測では OS の中で
         * カーネルを 1 本コンパイルするたびに約 18MB 漏れ、512MB を
         * 15 本で使い切っていた。
         *
         * pmm_free は参照カウントを見るので、共有されているページを
         * ここで返しても取り上げてしまうことはない。
         *
         * get_phys はページ内オフセットを OR して返すアーキがあるので、
         * **下位ビットを落としてから渡す** */
        uint64_t phys = arch_vm_get_phys(address_space, base + off);
        arch_vm_unmap_page(address_space, base + off);
        if (phys) pmm_free((void*)(uintptr_t)(phys & ~(PAGE_SIZE - 1ULL)), 1);
    }
    arch_syscall_flush_tlb();
    return 0;
}

static int linux_bootstrap_sys_set_tid_address(int* tidptr) {
    struct task* current = get_current_task();
    (void)tidptr;
    return current ? current->pid : -LINUX_ESRCH;
}

static int linux_bootstrap_sys_futex(volatile int* uaddr, int op, int val) {
    int cmd = op & ~FUTEX_PRIVATE;
    if (!uaddr) return -LINUX_EFAULT;
    switch (cmd) {
        case FUTEX_WAIT:
            return (*uaddr == val) ? 0 : -LINUX_EAGAIN;
        case FUTEX_WAKE:
            return 0;
        default:
            /* 未対応の futex 操作。EPERM だと呼び出し側が「権限が無い」と
             * 誤解するので、実装が無いことを ENOSYS で伝える */
            return -LINUX_ENOSYS;
    }
}

/* CLOCK_REALTIME (0) と CLOCK_MONOTONIC (1)。
 *
 * **REALTIME は xv6fs の時計と同じものを返すこと。**別々にすると、
 * ファイルの mtime だけが未来にある状態になり、make が
 *
 *   File 'Makefile' has modification time NNNN s in the future
 *   Clock skew detected.  Your build may be incomplete.
 *
 * を全ファイルについて出す (2026-08-28 に実機で確認)。この機械に RTC は
 * 無いので、**唯一の「壁時計らしきもの」は xv6fs が持つ土台**である。
 * それを唯一の出どころにして、時計が 2 つある状態を作らない。
 *
 * MONOTONIC のほうは起動からの経過でよい。むしろ土台を混ぜてはいけない
 * (mount のたびに飛ぶので単調でなくなる)。 */
static int linux_bootstrap_sys_clock_gettime(int clock_id, struct linux_timespec* ts) {
    uint64_t ms;
    if (!ts) return -LINUX_EFAULT;
    if (clock_id != 0 && clock_id != 1) return -LINUX_EINVAL;
    ms = arch_time_now_ms();
    ts->tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL);
    /* xv6fs が未マウントなら土台は 0 で、経過秒そのものに退く */
    ts->tv_sec = (clock_id == 0) ? (int64_t)xv6fs_now_sec()
                                 : (int64_t)(ms / 1000ULL);
    return 0;
}

/* nanosleep(2)。ms 解像度しか無いので端数は切り上げる (0 を要求されない限り
 * 必ず 1 tick 以上眠らせる)。既存の sleep 機構 (task_mark_io_wait_until +
 * sched.c の起床走査) にそのまま載せる。 */
/* poll(2)/ppoll(2)。riscv64 に poll は無く、musl の poll() は ppoll(73) を出す。
 * busybox の行編集 (CONFIG_FEATURE_EDITING) が 1 文字ごとに呼ぶ。 */
#define LINUX_POLLIN   0x001
#define LINUX_POLLPRI  0x002
#define LINUX_POLLOUT  0x004
#define LINUX_POLLERR  0x008
#define LINUX_POLLHUP  0x010
#define LINUX_POLLNVAL 0x020

struct linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* 1 つの fd の現在の readiness。events でマスクした結果を返す */
static int16_t linux_poll_fd_revents(int fd, int16_t events) {
    struct task* current = get_current_task();
    file_descriptor_t* f;
    int16_t ready = 0;

    if (!current || fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) {
        return LINUX_POLLNVAL;
    }
    f = &current->fds[fd];
    switch (f->type) {
        case FT_CONSOLE:
            if (arch_console_has_input()) ready |= LINUX_POLLIN;
            ready |= LINUX_POLLOUT;  /* シリアル出力は常に受け付ける */
            break;
        case FT_PIPE: {
            pipe_t* pipe = (pipe_t*)f->data;
            if (!pipe) { ready |= LINUX_POLLERR; break; }
            {
                uint64_t flags = spin_lock_irqsave(&pipe->lock);
                if (pipe->count > 0) ready |= LINUX_POLLIN;
                if (pipe->count < PIPE_BUF_SIZE) ready |= LINUX_POLLOUT;
                /* 相手側が閉じた = 自分しか参照していない */
                if (pipe->ref_count <= 1) ready |= LINUX_POLLHUP;
                spin_unlock_irqrestore(&pipe->lock, flags);
            }
            break;
        }
        default:
            /* 通常ファイル / ディレクトリ / /dev/null 等は常に ready (POSIX 準拠) */
            ready |= LINUX_POLLIN | LINUX_POLLOUT;
            break;
    }
    /* POLLERR/POLLHUP/POLLNVAL は events に無くても返る */
    return (int16_t)((ready & (events | LINUX_POLLERR | LINUX_POLLHUP | LINUX_POLLNVAL)));
}

/* ppoll が待ちに入るときの上限 (ms)。イベントで起きるのが通常経路で、
 * ここが効くのは「寝る」と「待ち手登録」の間で取りこぼしたときだけ */
#define LINUX_PPOLL_SLICE_MS 100

/* 待ち手として登録する。コンソールとパイプだけが待ち合わせを持つ
 * (通常ファイルなどは常に ready なのでここへ来ない) */
/* 全 fd の待ち行列へ自分を登録する。
 * 戻り値: 全部に登録できたら 1、1 つでも溢れたら 0 (呼び出し側が期限付きで寝る) */
static int linux_ppoll_register_waiters(struct linux_pollfd* fds, uint64_t nfds,
                                          struct task* self) {
    struct task* current = get_current_task();
    int all_registered = 1;
    if (!current || !self) return 0;
    for (uint64_t i = 0; i < nfds; i++) {
        file_descriptor_t* f;
        int fd = fds[i].fd;
        if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) continue;
        f = &current->fds[fd];
        if (f->type == FT_CONSOLE) {
            if (!arch_console_set_waiter(self)) all_registered = 0;
        } else if (f->type == FT_PIPE && f->data) {
            pipe_t* pipe = (pipe_t*)f->data;
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            if (fds[i].events & LINUX_POLLIN) {
                if (!fs_waitq_add(&pipe->read_wq, self)) all_registered = 0;
            }
            if (fds[i].events & LINUX_POLLOUT) {
                if (!fs_waitq_add(&pipe->write_wq, self)) all_registered = 0;
            }
            spin_unlock_irqrestore(&pipe->lock, flags);
        }
    }
    return all_registered;
}

static void linux_ppoll_clear_waiters(struct linux_pollfd* fds, uint64_t nfds,
                                        struct task* self) {
    struct task* current = get_current_task();
    if (!current || !self) return;
    for (uint64_t i = 0; i < nfds; i++) {
        file_descriptor_t* f;
        int fd = fds[i].fd;
        if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) continue;
        f = &current->fds[fd];
        if (f->type == FT_CONSOLE) {
            arch_console_clear_waiter(self);
        } else if (f->type == FT_PIPE && f->data) {
            pipe_t* pipe = (pipe_t*)f->data;
            uint64_t flags = spin_lock_irqsave(&pipe->lock);
            fs_waitq_remove(&pipe->read_wq, self);
            fs_waitq_remove(&pipe->write_wq, self);
            spin_unlock_irqrestore(&pipe->lock, flags);
        }
    }
}

/* 全 fd の revents を埋め、ready だった数を返す。何度呼んでも同じ (冪等) */
static int linux_ppoll_scan(struct linux_pollfd* fds, uint64_t nfds) {
    int ready_count = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        int16_t revents;
        if (fds[i].fd < 0) {          /* 負の fd は無視する規約 */
            fds[i].revents = 0;
            continue;
        }
        revents = linux_poll_fd_revents(fds[i].fd, fds[i].events);
        fds[i].revents = revents;
        if (revents != 0) ready_count++;
    }
    return ready_count;
}

static int64_t linux_bootstrap_sys_ppoll(struct linux_pollfd* fds, uint64_t nfds,
                                           const struct linux_timespec* timeout) {
    struct task* current = get_current_task();
    uint64_t deadline = 0;
    int has_deadline = 0;

    if (nfds > MAX_FDS) return -LINUX_EINVAL;
    if (nfds != 0 && !fds) return -LINUX_EFAULT;

    if (timeout) {
        uint64_t ms = (uint64_t)timeout->tv_sec * 1000ULL +
                      ((uint64_t)timeout->tv_nsec + 999999ULL) / 1000000ULL;
        deadline = arch_time_now_ms() + ms;
        has_deadline = 1;
    }

    for (;;) {
        int ready_count;
        int registered;
        uint64_t now;

        ready_count = linux_ppoll_scan(fds, nfds);
        if (ready_count > 0) return ready_count;
        now = arch_time_now_ms();
        if (has_deadline && now >= deadline) return 0;
        if (!current) {
            /* タスクがまだ無い文脈では寝られない */
            kernel_yield();
            continue;
        }

        /*
         * 待ちに入る手順。以前は「寝る状態にしてから登録」で、その間に来た
         * イベントを取りこぼすため 100ms で必ず起き直していた (アイドル時に
         * 10Hz で回る)。登録先が fd ごとの待ち行列になったので閉じられる:
         *
         *   1. 先に全 fd の待ち行列へ登録する (自分はまだ RUNNING)
         *   2. 寝る状態にする
         *   3. **もう一度**条件を見る            <- ここが競合を閉じる
         *   4. yield
         *
         * 1 より後に来たイベントは、登録済みなので必ず task_wake が飛んで
         * READY に戻される。2 と 3 の間に来た分も同じ。3 で条件が揃っていれば
         * 自分で就寝を取り消して先頭へ戻る。どの順で挟まれても取りこぼさない。
         *
         * 期限は呼び出し側が timeout を指定したときだけ持つ。指定が無ければ
         * 期限なしで寝る (定期的な起き直しはしない)。
         */
        registered = linux_ppoll_register_waiters(fds, nfds, current);
        if (has_deadline) {
            task_mark_io_wait_until(current, deadline);
        } else if (registered) {
            task_mark_io_wait(current);
        } else {
            /* 待ち行列が溢れて登録できなかった fd がある。この 1 回だけは
             * 期限を持たせて起き直す (溢れの保険であって、通常の経路ではない) */
            task_mark_io_wait_until(current, now + LINUX_PPOLL_SLICE_MS);
        }

        if (linux_ppoll_scan(fds, nfds) > 0) {
            /* task_wake() ではなく task_cancel_sleep()。あちらは runqueue へ
             * 積むので、走行中の自分が runqueue にも載って二重になる */
            task_cancel_sleep(current);
        }
        /* 取り消した場合も yield は必ず通す。誰かが先に起こしていて既に
         * runqueue へ載っているときは、ここで正規の経路から選び直される */
        kernel_yield();
        linux_ppoll_clear_waiters(fds, nfds, current);
    }
}

static int linux_bootstrap_sys_nanosleep(const struct linux_timespec* req,
                                           struct linux_timespec* rem) {
    struct task* current = get_current_task();
    int64_t req_sec;
    int64_t req_nsec;
    uint64_t ms;
    uint64_t deadline;

    if (!req) return -LINUX_EFAULT;
    /* musl の sleep() は nanosleep(&tv, &tv) と req と rem に同じポインタを渡す。
     * rem を先に書くと要求時間を自分で潰すので、必ず req を退避してから触ること */
    req_sec = req->tv_sec;
    req_nsec = req->tv_nsec;
    if (req_sec < 0 || req_nsec < 0 || req_nsec >= 1000000000L) return -LINUX_EINVAL;
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    ms = (uint64_t)req_sec * 1000ULL + ((uint64_t)req_nsec + 999999ULL) / 1000000ULL;
    if (ms == 0 || !current) {
        kernel_yield();
        return 0;
    }
    deadline = arch_time_now_ms() + ms;
    /* 早すぎる起床 (console 待ちの起床など、sleep_until_ms と無関係な経路から
     * READY にされる) があり得るので、デッドラインを再確認して寝直す。
     * 起床は sched.c の task_on_timer_tick() の走査が行う。 */
    while (arch_time_now_ms() < deadline) {
        task_mark_io_wait_until(current, deadline);
        kernel_yield();
    }
    return 0;
}

static int64_t linux_bootstrap_sys_ioctl(int fd, unsigned long request, uint64_t arg) {
    switch (request) {
        case LINUX_TIOCGWINSZ:
            if (!arg) return -LINUX_EFAULT;
            ((struct linux_winsize*)(uintptr_t)arg)->ws_row = 25;
            ((struct linux_winsize*)(uintptr_t)arg)->ws_col = 80;
            ((struct linux_winsize*)(uintptr_t)arg)->ws_xpixel = 0;
            ((struct linux_winsize*)(uintptr_t)arg)->ws_ypixel = 0;
            return 0;
        case LINUX_TIOCGPGRP:
            if (!arg) return -LINUX_EFAULT;
            if (g_linux_tty_pgrp == 0) {
                struct task* current = get_current_task();
                if (current) g_linux_tty_pgrp = current->pgid;
            }
            *(int*)(uintptr_t)arg = g_linux_tty_pgrp;
            return 0;
        case LINUX_TIOCSPGRP:
            if (!arg) return -LINUX_EFAULT;
            g_linux_tty_pgrp = *(const int*)(uintptr_t)arg;
            return 0;
        case LINUX_TCGETS:
            if (!arg) return -LINUX_EFAULT;
            *(struct linux_termios*)(uintptr_t)arg = g_linux_console_termios;
            return 0;
        case LINUX_TCSETS:
            if (!arg) return -LINUX_EFAULT;
            g_linux_console_termios = *(const struct linux_termios*)(uintptr_t)arg;
            return 0;
        default:
            (void)fd;
            /* コンソール以外の ioctl は無い。ENOTTY は musl/busybox が
             * 「tty ではない」と解釈して素通りできる唯一の値 */
            return -LINUX_ENOTTY;
    }
}

static int linux_bootstrap_sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset, size_t sigsetsize) {
    struct task* current = get_current_task();
    uint64_t newmask;
    if (!current) return -LINUX_ESRCH;
    if (sigsetsize != sizeof(uint64_t)) return -LINUX_EINVAL;
    if (oldset) *oldset = current->sig_mask;
    if (!set) return 0;
    newmask = *set;
    switch (how) {
        case LINUX_SIG_BLOCK:
            current->sig_mask |= newmask;
            break;
        case LINUX_SIG_UNBLOCK:
            current->sig_mask &= ~newmask;
            break;
        case LINUX_SIG_SETMASK:
            current->sig_mask = newmask;
            break;
        default:
            return -LINUX_EINVAL;
    }
    return 0;
}

static int linux_bootstrap_sys_rt_sigaction(int sig, const struct linux_sigaction* act,
                                              struct linux_sigaction* oldact, size_t sigsetsize) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    if (sig <= 0 || sig >= 32) return -LINUX_EINVAL;
    if (sigsetsize != sizeof(uint64_t)) return -LINUX_EINVAL;
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

static int64_t linux_bootstrap_sys_getrandom(void* buf, size_t len, unsigned flags) {
    uint8_t* out = (uint8_t*)buf;
    uint64_t seed;
    (void)flags;
    if (!out) return -LINUX_EFAULT;
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

static int64_t linux_bootstrap_sys_waitid(int idtype, int id, struct linux_siginfo* infop,
                                            int options) {
    int status = 0;
    int wait_pid;
    int target = -1;

    if (idtype == 0) target = id;
    else if (idtype == 1) target = -1;
    else return -LINUX_EINVAL;

    wait_pid = (int)linux_bootstrap_sys_wait4(target, &status, options);
    if (wait_pid < 0) return wait_pid;
    if (infop) {
        for (size_t i = 0; i < sizeof(*infop); i++) ((uint8_t*)infop)[i] = 0;
        infop->si_signo = 17;
        infop->si_code = 1;
    }
    return 0;
}

/* mmap(2) の失敗は「戻り値そのもの」が -errno になる (musl の __syscall_ret は
 * -4096 < ret < 0 を errno へ写す)。(void*)-1 を返すと MAP_FAILED ではなく
 * -EPERM を返したことになり、ユーザーには "Operation not permitted" として出る */
static void* linux_mmap_err(int err) {
    return (void*)(intptr_t)(-err);
}

/* **riscv64 は kernel/sys_fs.c を繋いでいない** (自前の kernel/riscv64/fs.c を
 * 使っており、sys_pread64 を持たない)。共有の mmap が file-backed を扱うのに
 * 要るので、既定を弱いシンボルで置いておく。riscv64 ではここが選ばれ、
 * ファイルを貼る mmap は ENOSYS で断られる。**riscv64 には共有 musl も
 * まだ無いので、いま困る利用者はいない。**そちらを繋ぐときに一緒に外す。
 * (sched.c の usb_hotplug_poll と同じ手) */
__attribute__((weak)) int64_t sys_pread64(int fd, void* buf, size_t count, int64_t offset) {
    (void)fd; (void)buf; (void)count; (void)offset;
    return -1;
}

/* ページ 1 枚をファイルから読んで埋める。**private マップなので写しでよい** —
 * 書き戻しは要らないし、他のプロセスと共有もしない (x86 の
 * copy_mmap_file_page と同じ作り)。
 * **pread を使うので fd の現在位置は動かない。**musl は同じ fd で
 * ヘッダを読みながらセグメントを貼るので、位置を動かすと壊れる */
static int linux_mmap_fill_from_file(uint8_t* dest, int fd, uint64_t file_off) {
    if (!dest || fd < 0) return -1;
    return (sys_pread64(fd, dest, PAGE_SIZE, (int64_t)file_off) < 0) ? -1 : 0;
}

/* mmap(2)。
 *
 * **2026-08-29 に file-backed / MAP_FIXED / PROT_EXEC を足した。**
 * それまでは無名 private だけで、共有ライブラリを 1 つも開けなかった。
 * musl の動的リンカ (ldso/dynlink.c:map_library) は
 *
 *   1. まず PROT_NONE で全体を予約する (無名)
 *   2. その中へ MAP_FIXED でセグメントをファイルから貼る
 *
 * という順で使うので、**3 つとも無いと "Invalid argument" で止まる**
 * (aarch64 で実測)。とくに PROT_EXEC は、無いとテキストが実行不可の
 * まま貼られて、開けても呼んだ瞬間に落ちる。
 *
 * x86 は kernel/sys_vm.c の sys_mmap が別に持っている (そちらが本家)。 */
/* この機械でファイルを貼る mmap が使えるか。弱い既定が選ばれていれば使えない */
static int linux_mmap_can_map_file(void) {
    /* pread が -1 を返すだけの既定かどうかは呼んでみないと分からないので、
     * 明らかに不正な fd で 1 回試す。本物は EBADF を返し、既定も -1 を
     * 返すため区別できない —— そこで **アーキで直に分ける** */
#if defined(__riscv)
    return 0;
#else
    return 1;
#endif
}

static void* linux_bootstrap_sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset) {
    struct task* current = get_current_task();
    arch_address_space_t address_space;
    uint64_t base;
    /* Sv39 のユーザー VA 上限 (2^38)。スタック領域 (0x3FFFFxxxxx) 手前まで */
    uint64_t limit = 0x0000003F00000000ULL;
    uint64_t size;
    uint64_t map_flags;
    int is_anonymous;

    if (!current) return linux_mmap_err(LINUX_ESRCH);
    if (length == 0) return linux_mmap_err(LINUX_EINVAL);
    if ((flags & MAP_PRIVATE) == 0) return linux_mmap_err(LINUX_EINVAL);
    if (offset < 0 || (offset & (PAGE_SIZE - 1)) != 0) return linux_mmap_err(LINUX_EINVAL);

    is_anonymous = (flags & MAP_ANONYMOUS) != 0;
    if (is_anonymous) {
        if (fd != -1 && fd != 0) return linux_mmap_err(LINUX_EINVAL);
    } else {
        if (fd < 0 || fd >= MAX_FDS || !current->fds[fd].in_use) return linux_mmap_err(LINUX_EBADF);
        /* **貼れないなら先に断る。**途中まで貼ってから諦めると後始末が要る */
        /* **ENOSYS で返す。**「この機械では実装が無い」の意味そのもので、
         * musl は静的な道へ退くか、意味の分かる文言を出せる */
        if (linux_mmap_can_map_file() == 0) return linux_mmap_err(LINUX_ENOSYS);
    }

    size = linux_align_up_page((uint64_t)length);
    if (!size) return linux_mmap_err(LINUX_EINVAL);

    address_space = arch_task_context_get_address_space(&current->ctx);

    if (flags & MAP_FIXED) {
        /* **要求された番地にそのまま貼る。**musl は 1 の予約で得た範囲の
         * 中を指してくるので、既に張られていれば置き換える */
        base = (uint64_t)(uintptr_t)addr;
        if (base & (PAGE_SIZE - 1)) return linux_mmap_err(LINUX_EINVAL);
        if (base == 0 || base + size < base) return linux_mmap_err(LINUX_EINVAL);
    } else {
        base = current->mmap_end;
        if (base < LINUX_USER_MMAP_BASE_VADDR) base = LINUX_USER_MMAP_BASE_VADDR;
        base = linux_align_up_page(base);
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
        /* ユーザー VA を使い切った */
        if (base + size > limit) return linux_mmap_err(LINUX_ENOMEM);
    }

    map_flags = arch_vm_user_page_flags((prot & PROT_WRITE) != 0,
                                        (prot & PROT_EXEC) != 0);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys;
        /* MAP_FIXED で既に張られている枠は、いったん剥がさず上書きする。
         * 元の物理ページは予約 (無名) のものなので使い回してよい */
        phys = (flags & MAP_FIXED) ? arch_vm_get_phys(address_space, base + off) : 0;
        if (!phys) {
            phys = linux_alloc_zeroed_user_page();
            if (!phys) return linux_mmap_err(LINUX_ENOMEM);
        } else {
            memset((void*)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        }
        if (!is_anonymous) {
            (void)linux_mmap_fill_from_file((uint8_t*)(uintptr_t)PHYS_TO_VIRT(phys),
                                            fd, (uint64_t)offset + off);
        }
        arch_vm_map_page(address_space, base + off, phys, map_flags);
    }
    if (base + size > current->mmap_end) current->mmap_end = base + size;
    arch_syscall_flush_tlb();
    /* **貼ったばかりのテキストを実行させる前に命令キャッシュを揃える。**
     * aarch64 は I/D が一貫していないので、揃えないと古い中身を実行しうる */
    if (prot & PROT_EXEC) arch_sync_icache_range((void*)(uintptr_t)base, size);
    return (void*)(uintptr_t)base;
}

/* N-6 (2026-08-31): mremap (216)。**未実装のままだと musl の realloc が
 * malloc+memcpy+free に退く。**動きは正しいが、ld の起動時など大きい
 * ヒープ塊を伸ばすたびに毎回これを踏んで遅くなっていた。
 *
 * kernel/sys_vm.c にも x86 ネイティブ ABI 向けの sys_mremap があるが、
 * その関数自体は aarch64/riscv64 のビルドに入っていない (arch_vm_* 抽象を
 * 使っていない、x86 専用の vmm_get_phys 等を直接呼ぶ実装のため)。ここでは
 * 同じ考え方を、この ABI が既に持っている arch_vm_* / mmap / munmap の
 * 部品で書き直した。 */
#define LINUX_MREMAP_MAYMOVE 1
#define LINUX_MREMAP_FIXED   2

static void* linux_bootstrap_sys_mremap(void* old_addr, size_t old_len, size_t new_len,
                                        int flags, void* new_addr) {
    struct task* current = get_current_task();
    arch_address_space_t address_space;
    uint64_t old_base = (uint64_t)(uintptr_t)old_addr;
    uint64_t old_size, new_size;

    if (!current) return linux_mmap_err(LINUX_ESRCH);
    if (!old_base || !old_len || !new_len) return linux_mmap_err(LINUX_EINVAL);
    if ((old_base & (PAGE_SIZE - 1)) != 0) return linux_mmap_err(LINUX_EINVAL);
    if (flags & ~(LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED)) return linux_mmap_err(LINUX_EINVAL);
    if ((flags & LINUX_MREMAP_FIXED) && !(flags & LINUX_MREMAP_MAYMOVE)) return linux_mmap_err(LINUX_EINVAL);

    old_size = linux_align_up_page((uint64_t)old_len);
    new_size = linux_align_up_page((uint64_t)new_len);
    address_space = arch_task_context_get_address_space(&current->ctx);

    if (!arch_vm_get_phys(address_space, old_base)) return linux_mmap_err(LINUX_EINVAL);

    if (new_size == old_size) return old_addr;

    /* 縮める: はみ出した分を外すだけ (munmap と同じ後始末) */
    if (new_size < old_size) {
        for (uint64_t off = new_size; off < old_size; off += PAGE_SIZE) {
            uint64_t phys = arch_vm_get_phys(address_space, old_base + off);
            arch_vm_unmap_page(address_space, old_base + off);
            if (phys) pmm_free((void*)(uintptr_t)(phys & ~(PAGE_SIZE - 1ULL)), 1);
        }
        arch_syscall_flush_tlb();
        return old_addr;
    }

    /* 伸ばす: 直後が空いていればその場で足す (move 不要) */
    {
        uint64_t end = old_base + old_size;
        uint64_t grow = new_size - old_size;
        uint64_t off;
        int occupied = 0;
        uint64_t map_flags;
        for (off = 0; off < grow; off += PAGE_SIZE) {
            if (arch_vm_get_phys(address_space, end + off) != 0) { occupied = 1; break; }
        }
        if (!occupied) {
            /* musl の realloc はヒープにしか使わない。読み書き可・実行不可を仮定する
             * (arch_vm_* に既存範囲の保護属性を取り直す手段が無い) */
            map_flags = arch_vm_user_page_flags(1, 0);
            for (off = 0; off < grow; off += PAGE_SIZE) {
                uint64_t phys = linux_alloc_zeroed_user_page();
                if (!phys) return linux_mmap_err(LINUX_ENOMEM);
                arch_vm_map_page(address_space, end + off, phys, map_flags);
            }
            arch_syscall_flush_tlb();
            return old_addr;
        }
    }

    if (!(flags & LINUX_MREMAP_MAYMOVE)) return linux_mmap_err(LINUX_ENOMEM);

    /* 直後に空きが無い: 新しい範囲を作り、中身を写してから古い方を外す */
    {
        void* mapped = linux_bootstrap_sys_mmap((flags & LINUX_MREMAP_FIXED) ? new_addr : 0,
                                                 (size_t)new_size, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS |
                                                 ((flags & LINUX_MREMAP_FIXED) ? MAP_FIXED : 0),
                                                 -1, 0);
        uint64_t new_base;
        uint64_t off;
        if ((int64_t)(intptr_t)mapped < 0 && (int64_t)(intptr_t)mapped > -4096) return mapped;
        new_base = (uint64_t)(uintptr_t)mapped;
        for (off = 0; off < old_size && off < new_size; off += PAGE_SIZE) {
            uint64_t old_phys = arch_vm_get_phys(address_space, old_base + off);
            uint64_t new_phys = arch_vm_get_phys(address_space, new_base + off);
            if (old_phys && new_phys) {
                memcpy((void*)(uintptr_t)PHYS_TO_VIRT(new_phys & ~(PAGE_SIZE - 1ULL)),
                       (void*)(uintptr_t)PHYS_TO_VIRT(old_phys & ~(PAGE_SIZE - 1ULL)), PAGE_SIZE);
            }
        }
        (void)linux_bootstrap_sys_munmap(old_addr, old_len);
        return (void*)(uintptr_t)new_base;
    }
}

static int linux_bootstrap_sys_mprotect(void* addr, size_t length, int prot) {
    struct task* current = get_current_task();
    arch_address_space_t address_space;
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t size;
    uint64_t flags;

    if (!current) return -LINUX_ESRCH;
    /* Linux は addr がページ境界でないと EINVAL。length 0 は成功 */
    if (base & (PAGE_SIZE - 1)) return -LINUX_EINVAL;
    if (length == 0) return 0;

    size = linux_align_up_page((uint64_t)length);
    if (!size || base + size < base) return -LINUX_EINVAL;

    address_space = arch_task_context_get_address_space(&current->ctx);
    flags = arch_vm_user_page_flags((prot & PROT_WRITE) != 0,
                                    (prot & PROT_EXEC) != 0);

    /* **先に全域が張られていることを確かめる。**途中まで書き換えてから
     * 穴に当たると、成功した分が戻せない。Linux も穴があれば ENOMEM */
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (arch_vm_get_phys(address_space, base + off) == 0) return -LINUX_ENOMEM;
    }
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys = arch_vm_get_phys(address_space, base + off);
        arch_vm_map_page(address_space, base + off, phys, flags);
    }
    arch_syscall_flush_tlb();
    /* 実行可にしたなら、命令キャッシュを揃えないと古い中身を実行しうる */
    if (prot & PROT_EXEC) arch_sync_icache_range((void*)(uintptr_t)base, size);
    return 0;
}

static int64_t linux_bootstrap_sys_wait4(int pid, int* wstatus, int options) {
    struct task* current = get_current_task();
    struct linux_child_wait w;

    if (!current) return -LINUX_ESRCH;
    w.ppid = current->pid;
    w.want = pid;

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
        /* 待つべき子がいない。ash のジョブ回収は wait4 が ECHILD を返すまで
         * 回すので、ここを EPERM にすると回収ループが止まらない */
        if (!found_child) return -LINUX_ECHILD;
        /* 子は居るがまだ終わっていない。WNOHANG なら 0 を返すのが POSIX */
        if (options & LINUX_WNOHANG) return 0;
        /* **焼かずに寝る。**子の exit で起こされるか、遅くとも
         * LINUX_WAIT4_POLL_MS で自力で起きる */
        (void)wait_event_timeout(&g_child_exit_wq, linux_child_zombie_ready,
                                 &w, LINUX_WAIT4_POLL_MS);
    }
}

static void linux_bootstrap_sys_exit(int status);

/* **ユーザーが落ちたときにも使う (S-1)。**アーキ側の例外ハンドラから
 * 呼べるよう外に出した。**戻らない。**
 *
 * 2026-08-22 の実機で、EL0 の命令アボートを起こしたプロセスが殺されず、
 * **同じ例外を毎秒 58 回上げ続けて電源断でしか止まらなかった。**
 * 落とすところまでを 1 か所に集める */
void linux_task_kill_current(int status) {
    linux_bootstrap_sys_exit(status);
}

static void linux_bootstrap_sys_exit(int status) {
    struct task* current = get_current_task();

    if (!current || current->ppid == 0) {
        (void)status;
        puts("  bootstrap user exit\n");
        arch_halt_forever();
    }

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd].in_use) {
            (void)sys_close(fd);
        }
    }

    /* M-8 (2026-08-31): 自分の子を pid 1 に引き取らせる (孤児 zombie が
     * 居座り続ける経緯を断つ)。詳細は linux_reap_orphans_of のコメント */
    linux_reap_orphans_of(current->pid);

    (void)task_mark_zombie(current, status);
    linux_notify_parent_exit(current);
    while (1) kernel_yield();
}

/*
 * ディスパッチは riscv64 (asm-generic) の番号だけを見る。
 *
 * かつては x86 レガシー番号 (include/syscall.h の SYS_*) も `case` に混ぜていたが、
 * 番号空間が無関係なので衝突が起き、実害が 2 度出た:
 *   - SYS_FORK(57) vs close(57)       → `close(0)` が fork として実行された
 *   - SYS_GETDENTS(78) vs readlinkat  → realpath が壊れた
 * 衝突を引数のヒューリスティックで分離していた箇所 (newfstatat/fstat/getdents64)
 * も、レガシー番号を落としたことで全部消せた。riscv64 のユーザーランドは musl
 * のみで、musl は asm-generic 番号しか発行しない。
 */
static void linux_bootstrap_syscall_dispatch(arch_syscall_frame_t* frame) {
    uint64_t syscall_no;
    if (!frame) return;
    syscall_no = arch_syscall_number(frame);

    switch (syscall_no) {
        case LINUX_SYS_WRITE:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_write((int)arch_syscall_arg0(frame),
                                                                                    (const void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (size_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_GETCWD:
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
        case LINUX_SYS_GETPID:
            {
                struct task* current = get_current_task();
                arch_syscall_set_return(frame, current ? (uint64_t)current->pid : 0);
                return;
            }
        /* **機械のリセット。** 焼き直しのたびに電源を抜かなくて済むように
         * 入れた。Linux と同じで魔法の数を 2 つ揃えないと効かない —
         * **誤爆で機械が落ちるのが一番困る**ので、この検査は削らないこと。
         *
         * 手段を持たないアーキ (riscv64 / x86_64) では -ENOSYS。
         * 成功したときはここから戻らない */
        case LINUX_SYS_REBOOT:
            {
                uint32_t magic1 = (uint32_t)arch_syscall_arg0(frame);
                uint32_t magic2 = (uint32_t)arch_syscall_arg1(frame);
                uint32_t cmd    = (uint32_t)arch_syscall_arg2(frame);
                if (magic1 != LINUX_REBOOT_MAGIC1 ||
                    (magic2 != LINUX_REBOOT_MAGIC2  && magic2 != LINUX_REBOOT_MAGIC2A &&
                     magic2 != LINUX_REBOOT_MAGIC2B && magic2 != LINUX_REBOOT_MAGIC2C)) {
                    arch_syscall_set_return(frame, (uint64_t)(int64_t)-22);   /* -EINVAL */
                    return;
                }
                if (cmd != LINUX_REBOOT_CMD_RESTART) {
                    /* halt / poweroff は Pi 4 では作れない (電源を落とす
                     * 経路が無い)。**出来ないことは出来ないと返す** */
                    arch_syscall_set_return(frame, (uint64_t)(int64_t)-38);   /* -ENOSYS */
                    return;
                }
                puts("\n[EL1] reboot: resetting the machine\r\n");
                if (arch_system_reset() < 0) {
                    arch_syscall_set_return(frame, (uint64_t)(int64_t)-38);   /* -ENOSYS */
                    return;
                }
                /* 仕掛けたが、カウントが尽きるまでの間ここに来る */
                for (;;) { }
            }
        case LINUX_SYS_GETPPID:
            {
                struct task* current = get_current_task();
                arch_syscall_set_return(frame, current ? (uint64_t)current->ppid : 0);
                return;
            }
        /* 単一ユーザー (root 固定)。/etc/passwd も root だけを持つ */
        case LINUX_SYS_GETUID:
        case LINUX_SYS_GETEUID:
        case LINUX_SYS_GETGID:
        case LINUX_SYS_GETEGID:
            arch_syscall_set_return(frame, 0);
            return;
        case LINUX_SYS_OPENAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_openat((int)arch_syscall_arg0(frame),
                                                                  (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                  (int)arch_syscall_arg2(frame),
                                                                  (int)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_READ:
            arch_syscall_set_return(frame,
                                    (uint64_t)sys_read((int)arch_syscall_arg0(frame),
                                                       (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                       (size_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_CLOSE:
            arch_syscall_set_return(frame, (uint64_t)(int64_t)sys_close((int)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_GETDENTS64:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_getdents64((int)arch_syscall_arg0(frame),
                                                                      (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                      (size_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_PPOLL:
            arch_syscall_set_return(frame,
                                    (uint64_t)linux_bootstrap_sys_ppoll(
                                        (struct linux_pollfd*)(uintptr_t)arch_syscall_arg0(frame),
                                        arch_syscall_arg1(frame),
                                        (const struct linux_timespec*)(uintptr_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_READLINKAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)linux_sys_readlinkat(
                                        (int)arch_syscall_arg0(frame),
                                        (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                        (char*)(uintptr_t)arch_syscall_arg2(frame),
                                        (size_t)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_SYMLINKAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)fs_symlinkat(
                                        (const char*)(uintptr_t)arch_syscall_arg0(frame),
                                        (int)arch_syscall_arg1(frame),
                                        (const char*)(uintptr_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_FSTAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_fstat_user((int)arch_syscall_arg0(frame),
                                                                              (struct linux_stat*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        /* riscv64 に stat(2) は無い。musl は newfstatat(AT_FDCWD, ...) を出す */
        case LINUX_SYS_NEWFSTATAT:
            {
                int dirfd = (int)arch_syscall_arg0(frame);
                const char* path = (const char*)(uintptr_t)arch_syscall_arg1(frame);
                struct linux_stat* st = (struct linux_stat*)(uintptr_t)arch_syscall_arg2(frame);
                int flags = (int)arch_syscall_arg3(frame);
                int rc;
                if (path && path[0] == '\0' && (flags & LINUX_AT_EMPTY_PATH) != 0) {
                    rc = linux_sys_fstat_user(dirfd, st);
                } else {
                    rc = linux_sys_fstatat_user(dirfd, path, st, flags);
                }
                arch_syscall_set_return(frame, (uint64_t)(int64_t)rc);
            }
            return;
        case LINUX_SYS_CHDIR:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_chdir((const char*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        /* 注: x86 レガシー番号の SYS_FCHDIR(81) は riscv64 の sync(81) と衝突するため
         * 採らない。musl が発行する asm-generic 番号 50 のみを受ける */
        case LINUX_SYS_FCHDIR:
            arch_syscall_set_return(frame, (uint64_t)(int64_t)sys_fchdir((int)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_MMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(uintptr_t)linux_bootstrap_sys_mmap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                    (size_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame),
                                                                                    (int)arch_syscall_arg3(frame),
                                                                                    (int)arch_syscall_arg4(frame),
                                                                                    (int64_t)arch_syscall_arg5(frame)));
            return;
        case LINUX_SYS_MUNMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_munmap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                     (size_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_MREMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(uintptr_t)linux_bootstrap_sys_mremap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                     (size_t)arch_syscall_arg1(frame),
                                                                                     (size_t)arch_syscall_arg2(frame),
                                                                                     (int)arch_syscall_arg3(frame),
                                                                                     (void*)(uintptr_t)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_BRK:
            arch_syscall_set_return(frame, linux_bootstrap_sys_brk(arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_MPROTECT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_mprotect((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                       (size_t)arch_syscall_arg1(frame),
                                                                                       (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_WAIT4:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_wait4((int)arch_syscall_arg0(frame),
                                                                                    (int*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_WAITID:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_waitid((int)arch_syscall_arg0(frame),
                                                                                    (int)arch_syscall_arg1(frame),
                                                                                    (struct linux_siginfo*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                    (int)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_SET_TID_ADDRESS:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_set_tid_address((int*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_FUTEX:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_futex((volatile int*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                    (int)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_NANOSLEEP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_nanosleep(
                                        (const struct linux_timespec*)(uintptr_t)arch_syscall_arg0(frame),
                                        (struct linux_timespec*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_CLOCK_GETTIME:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_clock_gettime((int)arch_syscall_arg0(frame),
                                                                                            (struct linux_timespec*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_RT_SIGACTION:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_rt_sigaction((int)arch_syscall_arg0(frame),
                                                                                           (const struct linux_sigaction*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                           (struct linux_sigaction*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                           (size_t)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_RT_SIGPROCMASK:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_rt_sigprocmask((int)arch_syscall_arg0(frame),
                                                                                             (const uint64_t*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                             (uint64_t*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                             (size_t)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_GETRANDOM:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_getrandom((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                        (size_t)arch_syscall_arg1(frame),
                                                                                        (unsigned)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_WRITEV:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_writev((int)arch_syscall_arg0(frame),
                                                                                    (const struct linux_iovec*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_READV:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_readv((int)arch_syscall_arg0(frame),
                                                                                   (const struct linux_iovec*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                   (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_LSEEK:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_lseek((int)arch_syscall_arg0(frame),
                                                                                   (int64_t)arch_syscall_arg1(frame),
                                                                                   (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_IOCTL:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_ioctl((int)arch_syscall_arg0(frame),
                                                                                   (unsigned long)arch_syscall_arg1(frame),
                                                                                   arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_EXECVE:
            {
                int rc = task_execve(frame,
                                     (const char*)(uintptr_t)arch_syscall_arg0(frame),
                                     (char* const*)(uintptr_t)arch_syscall_arg1(frame),
                                     (char* const*)(uintptr_t)arch_syscall_arg2(frame));
                if (rc < 0) {
                    /* 失敗の大半は「その実行ファイルが無い」なので -1 は -ENOENT に
                     * 写す。ただし引数が多すぎる場合は task_execve が -E2BIG を
                     * 返してくるので、それは潰さずに通す (busybox の xargs は
                     * E2BIG を見て分割するため、ENOENT にすると分割してくれない) */
                    int err = (rc == -LINUX_E2BIG) ? -LINUX_E2BIG : -LINUX_ENOENT;
                    arch_syscall_set_return(frame, (uint64_t)(int64_t)err);
                }
                /* 成功時は frame が新プロセスの初期状態に書き換わっている */
            }
            return;
        case LINUX_SYS_PIPE2:
            {
                extern int sys_pipe2(int* pipefd, int flags);
                arch_syscall_set_return(frame,
                                        (uint64_t)(int64_t)sys_pipe2((int*)(uintptr_t)arch_syscall_arg0(frame),
                                                                     (int)arch_syscall_arg1(frame)));
            }
            return;
        case LINUX_SYS_DUP:
            {
                extern int sys_dup(int oldfd);
                arch_syscall_set_return(frame,
                                        (uint64_t)(int64_t)sys_dup((int)arch_syscall_arg0(frame)));
            }
            return;
        case LINUX_SYS_DUP3:
            {
                extern int sys_dup3(int oldfd, int newfd, int flags);
                arch_syscall_set_return(frame,
                                        (uint64_t)(int64_t)sys_dup3((int)arch_syscall_arg0(frame),
                                                                    (int)arch_syscall_arg1(frame),
                                                                    (int)arch_syscall_arg2(frame)));
            }
            return;
        case LINUX_SYS_FCNTL:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_fcntl((int)arch_syscall_arg0(frame),
                                                                 (int)arch_syscall_arg1(frame),
                                                                 arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_MKDIRAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_mkdirat((int)arch_syscall_arg0(frame),
                                                                   (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                   (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_UNLINKAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_unlinkat((int)arch_syscall_arg0(frame),
                                                                    (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_LINKAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_linkat((int)arch_syscall_arg0(frame),
                                                                  (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                  (int)arch_syscall_arg2(frame),
                                                                  (const char*)(uintptr_t)arch_syscall_arg3(frame),
                                                                  (int)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_STATFS:
        case LINUX_SYS_FSTATFS:
            {
                struct orth_statfs st;
                struct linux_statfs* u =
                    (struct linux_statfs*)(uintptr_t)arch_syscall_arg1(frame);
                int rc;
                if (syscall_no == LINUX_SYS_STATFS) {
                    rc = sys_statfs((const char*)(uintptr_t)arch_syscall_arg0(frame), &st);
                } else {
                    rc = sys_fstatfs((int)arch_syscall_arg0(frame), &st);
                }
                if (rc == 0 && u) {
                    memset(u, 0, sizeof(*u));
                    u->f_type    = LINUX_XV6FS_MAGIC;
                    u->f_bsize   = st.bsize;
                    u->f_blocks  = st.blocks;
                    u->f_bfree   = st.bfree;
                    u->f_bavail  = st.bavail;
                    u->f_files   = st.files;
                    u->f_ffree   = st.ffree;
                    u->f_namelen = st.namelen;
                    u->f_frsize  = st.bsize;
                } else if (rc == 0 && !u) {
                    rc = -14;   /* EFAULT */
                }
                arch_syscall_set_return(frame, (uint64_t)(int64_t)rc);
            }
            return;
        case LINUX_SYS_UNAME:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_uname(
                                        (struct linux_utsname*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_GETRLIMIT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_getrlimit(
                                        (int)arch_syscall_arg0(frame),
                                        (struct linux_rlimit64*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_SETRLIMIT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_setrlimit(
                                        (int)arch_syscall_arg0(frame),
                                        (const struct linux_rlimit64*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_PRLIMIT64:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_prlimit64(
                                        (int)arch_syscall_arg0(frame),
                                        (int)arch_syscall_arg1(frame),
                                        (const struct linux_rlimit64*)(uintptr_t)arch_syscall_arg2(frame),
                                        (struct linux_rlimit64*)(uintptr_t)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_TIMES:
            arch_syscall_set_return(frame,
                                    (uint64_t)linux_sys_times(
                                        (struct linux_tms*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_FCHOWN:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_fchown(
                                        (int)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_FCHOWNAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_fchownat(
                                        (int)arch_syscall_arg0(frame),
                                        (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                        (int)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_GETRUSAGE:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_getrusage(
                                        (int)arch_syscall_arg0(frame),
                                        (struct linux_rusage*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_UMASK:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_umask(
                                        (int)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_SYSINFO:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_sys_sysinfo(
                                        (struct linux_sysinfo*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_TRUNCATE:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_truncate((const char*)(uintptr_t)arch_syscall_arg0(frame),
                                                                    arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_FTRUNCATE:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_ftruncate((int)arch_syscall_arg0(frame),
                                                                     arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_FCHMODAT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_fchmodat((int)arch_syscall_arg0(frame),
                                                                                      (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                      (uint32_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_FCHMOD:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)linux_bootstrap_sys_fchmod((int)arch_syscall_arg0(frame),
                                                                                    (uint32_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_SYNC:
        case LINUX_SYS_FSYNC:
        case LINUX_SYS_FDATASYNC:
            arch_syscall_set_return(frame, (uint64_t)(int64_t)sys_sync());
            return;
        case LINUX_SYS_UTIMENSAT:
            /* **無条件に 0 を返してはいけない (2026-08-30)。**
             *
             * busybox の touch は「まず utimensat を試し、ENOENT なら
             * open(O_CREAT) で作る」という順で動く。ここが常に成功を返すと
             * **touch がファイルを作らなくなる。**
             *
             * GCC の configure がこれで落ちていた —— 依存形式の検査が
             * `touch sub/conftst$i.h` でヘッダを 6 つ用意するのに 1 つも
             * 作られず、14 方式すべてが「conftst1.h が無い」で失敗して
             * 「no usable dependency style found」になっていた。
             *
             * fs_utimensat は最初から**存在しなければ ENOENT を返す**
             * 正しい実装だった。呼んでいなかっただけ。
             * (元のコメントの「xv6fs はタイムスタンプを持たない」は、
             *  mtime を入れた時点で古くなっていた) */
            arch_syscall_set_return(frame,
                (uint64_t)(int64_t)sys_utimensat((int)arch_syscall_arg0(frame),
                                                 (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                 (const void*)(uintptr_t)arch_syscall_arg2(frame),
                                                 (int)arch_syscall_arg3(frame)));
            return;
        /* rename(2)。**番号がアーキで違うので両方受ける。**aarch64 の musl は
         * renameat(38)、riscv64 の musl は renameat2(276) を出す。
         * 38 を落としていたころは aarch64 で rename が丸ごと ENOSYS だった */
        case LINUX_SYS_RENAMEAT:
            arch_syscall_set_return(frame,
                (uint64_t)(int64_t)sys_renameat((int)arch_syscall_arg0(frame),
                                                (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                (int)arch_syscall_arg2(frame),
                                                (const char*)(uintptr_t)arch_syscall_arg3(frame),
                                                0U));
            return;
        case LINUX_SYS_RENAMEAT2:
            arch_syscall_set_return(frame,
                (uint64_t)(int64_t)sys_renameat((int)arch_syscall_arg0(frame),
                                                (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                (int)arch_syscall_arg2(frame),
                                                (const char*)(uintptr_t)arch_syscall_arg3(frame),
                                                (unsigned int)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_FACCESSAT:
            {
                struct kstat st;
                int dirfd = (int)arch_syscall_arg0(frame);
                const char* path = (const char*)(uintptr_t)arch_syscall_arg1(frame);
                int rc = sys_fstatat(dirfd, path, &st, 0);
                arch_syscall_set_return(frame, (uint64_t)(int64_t)(rc == 0 ? 0 : -2));
            }
            return;
        case LINUX_SYS_EXIT:
        case LINUX_SYS_EXIT_GROUP:
            linux_bootstrap_sys_exit((int)arch_syscall_arg0(frame));
            return;
        default:
            linux_report_unimplemented_syscall(arch_syscall_number(frame));
            arch_syscall_set_return(frame, (uint64_t)-38);
            return;
    }
}

/* アーキ固有の私物 syscall (ORTH_SYS_*)。**扱えたら 1 を返す。**
 *
 * 画面やキーのように「その機械にしか無いもの」はここで受ける。
 * **既定は「何も扱えない」**の弱いシンボルで、必要なアーキだけ上書きする
 * (kernel/fs.c の arch_console_echo_enabled と同じ形)。
 * riscv64 は上書きしていないので、従来どおり ENOSYS になる */
__attribute__((weak))
int arch_orth_syscall(arch_syscall_frame_t* frame, uint64_t number) {
    (void)frame; (void)number;
    return 0;
}

void linux_syscall_dispatch(arch_syscall_frame_t* frame) {
    if (!frame) return;

    /* **Linux の番号空間より上は先に横取りする。** ORTH_SYS_BASE = 1000 で、
     * 下の巨大な switch には入れない (番号がぶつからないようにしてある) */
    if (arch_syscall_number(frame) >= ORTH_SYS_BASE) {
        if (arch_orth_syscall(frame, arch_syscall_number(frame))) {
            arch_syscall_advance_pc(frame);
            return;
        }
        linux_report_unimplemented_syscall(arch_syscall_number(frame));
        arch_syscall_set_return(frame, (uint64_t)-38);
        arch_syscall_advance_pc(frame);
        return;
    }

    // fork は clone(SIGCHLD, 0) のみで受ける。riscv64 に fork(2) は無く、
    // musl の fork() は clone を出す。
    //
    // vfork も同じ入口で受ける。musl の riscv64 vfork は手書き asm で
    //   clone(CLONE_VM|CLONE_VFORK|SIGCHLD, sp)   (= a0=0x4111, a1=sp)
    // を出す (src/process/riscv64/vfork.s)。busybox の spawn() がこれを使うため、
    // 対応しないと xargs 等が ENOSYS で落ちる。
    // **アドレス空間は共有せず通常の fork としてコピーする**。vfork の子は直後に
    // exec するのが前提なので実用上は問題ないが、exec 前に子が書いた内容が親から
    // 見えない点だけ本来の vfork と異なる。親を停止させないのも同様に許容している
    // (呼び出し側は waitpid か pipe で同期するため)。
    // **a2 以降は見ないこと。** musl の _Fork.c は
    //   __syscall(SYS_clone, SIGCHLD, 0)
    // と引数を 2 つしか渡さないので、a2 は直前の呼び出し (__lock) が残した値になる。
    // 以前は a2 == 0 も条件に入れていたが、それはたまたま a2 が 0 だった
    // ビルドでしか成立しない。実際 GCC 4.7.4 で musl を組むと a2 に残留値が入り、
    // fork が ENOSYS で落ちた。Linux も flags に CLONE_PARENT_SETTID /
    // CLONE_CHILD_SETTID / CLONE_SETTLS が無ければ a2..a4 を参照しない。
    if (arch_syscall_number(frame) == LINUX_SYS_CLONE &&
        ((arch_syscall_arg0(frame) == LINUX_CLONE_SIGCHLD && arch_syscall_arg1(frame) == 0) ||
         arch_syscall_arg0(frame) == (LINUX_CLONE_VM | LINUX_CLONE_VFORK |
                       LINUX_CLONE_SIGCHLD))) {
        // 子は親フレームのコピーで復帰するため、先に sepc を進めて
        // 子が ecall を再実行しないようにする (親子とも次命令から再開)
        arch_syscall_advance_pc(frame);
        int ret = task_fork(frame);
        arch_syscall_set_user_return(frame, arch_syscall_program_counter(frame),
                                     arch_syscall_stack_pointer(frame),
                                     (uint64_t)(int64_t)ret,
                                     arch_syscall_arg1(frame), arch_syscall_arg2(frame));
        arch_syscall_sync_current_user_frame(frame);
        return;
    }

    // arch_syscall_frame_t はトラップフレームそのものなので直接ディスパッチする。
    // ecall の次命令から再開する既定値を先に設定し、execve 等が sepc/sp を上書きできるようにする。
    arch_syscall_advance_pc(frame);
    linux_syscall_trace_enter(frame);
    linux_bootstrap_syscall_dispatch(frame);
    linux_syscall_trace_leave(frame);
    arch_syscall_sync_current_user_frame(frame);
}

void arch_syscall_sync_current_user_frame(const arch_syscall_frame_t* frame) {
    task_context_t* ctx;
    if (!frame) return;
    ctx = task_current_context();
    if (!ctx) ctx = g_linux_fallback_current_context;
    if (!ctx) return;
    arch_task_store_user_frame_hook(ctx, frame);
}

void arch_syscall_set_current_context(struct arch_task_context* ctx) {
    g_linux_fallback_current_context = (task_context_t*)ctx;
}
