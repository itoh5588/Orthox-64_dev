#include <stdint.h>
#include "fs.h"
#include "stdio.h"
#include "pmm.h"
#include "vmm.h"
#include "linux_errno.h"
#include "linux_abi.h"
#include "version.h"
#include "spinlock.h"
#include "linux_syscalls.h"
#include "arch_syscall.h"
#include "arch_vm.h"   /* mprotect が権限ビットを組み立てる */
#include "string.h"
#include "linux_syscall.h"
#include "net_socket.h"
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

/* シグナル番号は include/linux_abi.h に出した (2026-09-09)。
 * **x86 側 (sys_proc.c) が SIGCHLD に 20 を使っていた**ので、置き場を
 * 1 つにして揃えた。 */

static struct task* linux_find_task_by_pid(int pid) {
    struct task* task = task_list;
    while (task) {
        if (task->pid == pid) return task;
        task = task->next;
    }
    return 0;
}

/* **wait4 の待ち行列は kernel/task.c に出した (2026-09-09)。**
 * ここにあった実装は 2026-08-30 に入れたもので、**x86 側 (sys_proc.c) には
 * 無く、あちらは kernel_yield() で回して待つ親がコアを 1 本焼いていた。**
 * 経緯と設計は kernel/task.c の task_child_exit_wake() のコメントを参照。 */

/* WNOHANG。**以前は options を捨てていた**ので、子がまだ終わっていない
 * ときに 0 を返さず待ち続けていた */
#define LINUX_WNOHANG 1

/*
 * 子の終了は、zombie 化だけでは待機中の親へ伝わらない。特に BusyBox ash の
 * $(cmd | cmd) はブロッキング waitpid に入り得るため、親を起こさないと
 * TASK_SLEEPING のまま残る。pipe の close より先にこの通知を行う必要はなく、
 * 親が wait4 を再走査した時点で child の zombie 状態が見えていればよい。
 */
static void linux_notify_parent_exit(struct task* child) {
    struct task* parent;
    if (!child || child->ppid <= 0) return;
    task_child_exit_wake();
    parent = linux_find_task_by_pid(child->ppid);
    if (!parent) return;
    parent->sig_pending |= (1ULL << LINUX_SIGCHLD);
    if (parent->state == TASK_SLEEPING) (void)task_wake(parent);
}

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


/* **uname は kernel/sys_uname.c へ移した (2026-09-13、別実装 29 組の 1 組)。**
 * 名乗る値を x86 と揃えた経緯 (2026-09-09) も移した先に書いてある。 */


/* ---- 資源の上限 (getrlimit / setrlimit / prlimit64) ----------------------
 *
 * **kernel/sys_rlimit.c へ移した (2026-09-13、別実装 29 組の 1 組)。**
 * x86 は全部 無限で答えていたので、共通化でそちらが直る側になる。
 * 経緯 (実測で ENOSYS が出たので 2026-08-11 に埋めた / 答えるだけで
 * 記憶しない / NOFILE と STACK は実際の値を答える理由) は移した先の
 * 冒頭に書いてある。 */

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
        task_reap_orphans_of(victim_pids[i]);
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




/* **brk(2) は kernel/sys_mmap.c へ移した (2026-09-12)。**
 * x86 (kernel/x86_64/sys_vm.c) と 2 実装あり、**ここには mmap の領域へ
 * 食い込ませない番人が無かった。**経緯は kernel/sys_mmap.c の sys_brk の冒頭 */
uint64_t sys_brk(uint64_t addr);

/* munmap は kernel/sys_mmap.c の sys_munmap (3 アーキ共通, 2026-09-11)。
 * ここに在った版は範囲を見ておらず、riscv64 ではユーザーからカーネルの
 * 写像を外せた。物理ページを返す理由の説明は sys_mmap.c の mmap_drop_page へ
 * 移した */



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
        case FT_SOCKET: {
            /* **2026-09-05 まで、ソケットは下の default: に落ちて
             * 「常に読める・書ける」を返していた。**musl の resolver が
             * poll -> recvmsg(EAGAIN) を空振りし続け、event loop で待つ
             * プログラムなら 100% 回りっぱなしになる。
             * 実際の状態は kernel/net_socket.c が知っている */
            int st = net_socket_poll_state(f);
            if (st < 0) { ready |= LINUX_POLLERR; break; }
            if (st & 1) ready |= LINUX_POLLIN;
            if (st & 2) ready |= LINUX_POLLOUT;
            if (st & 4) ready |= LINUX_POLLHUP;
            if (st & 8) ready |= LINUX_POLLERR;
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




/* ---- sendmsg(2) / recvmsg(2) (2026-09-05、TLS の手3) ----------------------
 *
 * **musl の resolver (src/network/res_msend.c) が recvmsg を使う。**
 * getaddrinfo -> res_msend -> recvmsg で、ここが無いと ENOSYS になり
 * 名前解決が一切できない (2026-09-05 に実測: "ENOSYS: syscall 0xd4")。
 *
 * **既に有る sendto/recvfrom の上に載せる。**下の層 (kernel/net_socket.c)
 * には手を入れない。
 *
 * **iovec は先頭の 1 本だけを扱う。**これは規格の範囲内 —— recvmsg は
 * 用意した領域より少なく返してよく、sendmsg も送れた分を返してよい。
 * musl の使い方はこれで足りる:
 *   - UDP の問い合わせは iovlen=1 (そのまま)
 *   - TCP の予備経路は iovlen=2 だが、musl 側が step_mh() で
 *     「送れた/受けた分」だけ iovec を進めて呼び直すので、1 本ずつでも進む
 * **足りないふりをしない。**扱えない範囲は下に明記する:
 *   - 補助データ (msg_control) は運ばない。要求されたら EINVAL で断る
 *     (黙って捨てると、受け取ったつもりの側が壊れる)
 *   - データグラムが先頭 iovec に収まらない場合、残りは捨てられる
 *     (recvfrom と同じ振る舞い)  */

/* struct linux_iovec は readv/writev のところで既に定義してある (上) */

struct linux_msghdr {
    void*   msg_name;
    uint32_t msg_namelen;
    uint32_t __pad0;
    struct linux_iovec* msg_iov;
    size_t  msg_iovlen;
    void*   msg_control;
    size_t  msg_controllen;
    int     msg_flags;
    uint32_t __pad1;
};

/* 先頭の「長さが 0 でない」iovec を返す。全部 0 なら 0 を返す */
static struct linux_iovec* linux_msg_first_iov(const struct linux_msghdr* mh) {
    if (!mh || !mh->msg_iov) return 0;
    for (size_t i = 0; i < mh->msg_iovlen; i++) {
        if (mh->msg_iov[i].iov_len != 0) return &mh->msg_iov[i];
    }
    return 0;
}

static int64_t linux_bootstrap_sys_sendmsg(int fd, struct linux_msghdr* mh, int flags) {
    struct linux_iovec* iov;
    if (!mh) return -LINUX_EFAULT;
    /* **補助データは運べない。**黙って落とすと、送ったつもりの側が壊れる */
    if (mh->msg_control && mh->msg_controllen) return -LINUX_EINVAL;
    iov = linux_msg_first_iov(mh);
    if (!iov) return 0;    /* 送るものが無い */
    return net_socket_sendto(fd, iov->iov_base, iov->iov_len, flags,
                             mh->msg_name, mh->msg_namelen);
}

static int64_t linux_bootstrap_sys_recvmsg(int fd, struct linux_msghdr* mh, int flags) {
    struct linux_iovec* iov;
    uint32_t namelen;
    int64_t got;

    if (!mh) return -LINUX_EFAULT;
    if (mh->msg_control && mh->msg_controllen) return -LINUX_EINVAL;
    iov = linux_msg_first_iov(mh);
    if (!iov) return 0;

    namelen = mh->msg_namelen;
    got = net_socket_recvfrom(fd, iov->iov_base, iov->iov_len, flags,
                              mh->msg_name, mh->msg_name ? &namelen : 0);
    if (got < 0) return got;
    /* **相手の番地の長さを書き戻す。**musl はこれを見て
     * 「送った先から返ってきたか」を照合する。書かないと答えを捨てる */
    if (mh->msg_name) mh->msg_namelen = namelen;
    mh->msg_controllen = 0;
    mh->msg_flags = 0;
    return got;
}

/* ---- getrandom(2) の乱数源 (2026-09-05 に差し替え) ------------------------
 *
 * **前はここに xorshift が直書きしてあった。**種は
 *
 *     arch_time_now_ms() ^ (タスク構造体のアドレス) ^ 定数
 *
 * で、**起動からの経過ミリ秒とカーネルの配置が分かれば再現できる**。
 * ふだんは実害が出ないが、TLS はここから鍵の材料を取るので、
 * 予測できる値を返すと「TLS の形はしているが守られていない」ものになる。
 *
 * 本物はアーキ側が出す (aarch64 は kernel/aarch64/rng.c —— 実機は
 * BCM2711 RNG200、QEMU virt は virtio-rng)。**riscv64 にはまだ無いので、
 * 弱いシンボルで「無い」を既定にする。**繋いでいないアーキで
 * リンクが落ちないようにするためで、sys_pread64 と同じ手 */
__attribute__((weak)) int64_t arch_random_bytes(void* buf, size_t len) {
    (void)buf; (void)len;
    return -1;
}

/* **必ず len バイト埋める側 (2026-09-09)。**
 * /dev/random と /dev/urandom の口。
 * x86 は kernel/x86_64/rng.c が強いシンボルで上書きする。
 *
 * **まず機械の源を試し、取れた分はそのまま使う。**足りない分だけ混ぜ物で
 * 作る。混ぜ物の材料は 2026-09-05 まで getrandom がここで使っていたものと
 * 同じで、**起動からの経過ミリ秒とカーネルの配置が分かれば再現できる。**
 * それでもこの口がエラーを返さないことを優先する —— Linux の /dev/urandom は
 * 読み手にエラーを返さず、呼び手 (CPython の起動など) は getrandom(2) が
 * ENOSYS のときここへ退避してくるため。**質を問う呼び手は getrandom(2) を
 * 使い、そちらは源が無ければ ENOSYS を返す。** */
__attribute__((weak)) void arch_random_fill(void* buf, size_t len) {
    static uint64_t state = 0;
    static int warned = 0;
    uint8_t* out = (uint8_t*)buf;
    size_t off = 0;
    int64_t got;

    if (!out || len == 0) return;

    got = arch_random_bytes(out, len);
    if (got == (int64_t)len) return;
    if (got > 0) off = (size_t)got;

    if (!warned) {
        warned = 1;
        puts("[rng] no hardware source; /dev/urandom is mixed, not random\r\n");
    }

    if (state == 0) {
        state = arch_time_now_ms() ^ (uint64_t)(uintptr_t)get_current_task()
                ^ 0x9E3779B97F4A7C15ULL;
    }
    while (off < len) {
        uint64_t word;
        size_t take;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        word = state * 0x2545F4914F6CDD1DULL;
        word ^= arch_time_now_ms();
        take = len - off;
        if (take > sizeof(word)) take = sizeof(word);
        for (size_t i = 0; i < take; i++) out[off + i] = (uint8_t)(word >> (i * 8));
        off += take;
    }
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

/* **mmap(2) は kernel/sys_mmap.c へ移した (2026-09-11)。**
 * x86 (kernel/x86_64/sys_vm.c) と 2 実装あり、**重複ではなく動作が
 * 違っていた** (MAP_SHARED / 失敗時の後始末 / PIPE の検査)。
 * **x86 側に寄せた** —— musl が MAP_SHARED を使うため。
 * 経緯と対照表は kernel/sys_mmap.c の冒頭にある */
void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd, int64_t offset);

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

/* **mremap(2) は kernel/sys_mmap.c へ移した (2026-09-12)。**
 * x86 (kernel/x86_64/sys_vm.c) と 2 実装あり、**保護の引き継ぎが違っていた**
 * (linux 側は移した先を必ず rw・実行不可にしていた)。
 * 経緯と対照表は kernel/sys_mmap.c の sys_mremap の冒頭にある */
void* sys_mremap(void* old_addr, size_t old_len, size_t new_len, int flags, void* new_addr);

/* **mprotect(2) は kernel/sys_mmap.c へ移した (2026-09-12)。**
 * x86 (kernel/x86_64/sys_vm.c) と 2 実装あり、**COW の扱いが違っていた。**
 * 経緯と対照表は kernel/sys_mmap.c の sys_mprotect の冒頭にある */
int sys_mprotect(void* addr, size_t length, int prot);

static int64_t linux_bootstrap_sys_wait4(int pid, int* wstatus, int options) {
    struct task* current = get_current_task();

    if (!current) return -LINUX_ESRCH;

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
         * TASK_CHILD_WAIT_POLL_MS で自力で起きる */
        task_wait_child_exit(current->pid, pid, TASK_CHILD_WAIT_POLL_MS);
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

    /* M-8 (2026-08-31): 自分の子を始末する (孤児 zombie が居座り続ける経緯を
     * 断つ)。**実装は kernel/task.c に出した (2026-09-08)** —— x86 側に同じ
     * ものが無く、実測で zombie が溜まっていたため。詳細はそちらのコメント */
    task_reap_orphans_of(current->pid);

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
                                    (uint64_t)(int64_t)sys_write((int)arch_syscall_arg0(frame),
                                                                                    (const void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (size_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_GETCWD:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_getcwd((char*)(uintptr_t)arch_syscall_arg0(frame),
                                                                  (size_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_GETPID:
            arch_syscall_set_return(frame, sys_getpid());
            return;
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
            arch_syscall_set_return(frame, sys_getppid());
            return;
        /* 単一ユーザー (root 固定)。値は kernel/sys_task.c */
        case LINUX_SYS_GETUID:
            arch_syscall_set_return(frame, sys_getuid());
            return;
        case LINUX_SYS_GETEUID:
            arch_syscall_set_return(frame, sys_geteuid());
            return;
        case LINUX_SYS_GETGID:
            arch_syscall_set_return(frame, sys_getgid());
            return;
        case LINUX_SYS_GETEGID:
            arch_syscall_set_return(frame, sys_getegid());
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
                                    (uint64_t)fs_readlinkat(
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
                                    (uint64_t)(uintptr_t)sys_mmap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                    (size_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame),
                                                                                    (int)arch_syscall_arg3(frame),
                                                                                    (int)arch_syscall_arg4(frame),
                                                                                    (int64_t)arch_syscall_arg5(frame)));
            return;
        case LINUX_SYS_MUNMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_munmap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                  (size_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_MREMAP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(uintptr_t)sys_mremap((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                     (size_t)arch_syscall_arg1(frame),
                                                                                     (size_t)arch_syscall_arg2(frame),
                                                                                     (int)arch_syscall_arg3(frame),
                                                                                     (void*)(uintptr_t)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_BRK:
            arch_syscall_set_return(frame, sys_brk(arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_MPROTECT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_mprotect((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                       (size_t)arch_syscall_arg1(frame),
                                                                                       (int)arch_syscall_arg2(frame)));
            return;
        /* ---- ソケット (N-10, 2026-09-04) ------------------------------
         *
         * kernel/net_socket.c (lwIP を後ろに持つ実装) をそのまま呼ぶ。
         * **read/write/close は既に通っている** —— kernel/fs.c が
         * FT_SOCKET を見て net_socket_read_fd/write_fd へ回すので、
         * ここで足りないのは入口だけだった。
         *
         * **返り値は net_socket.c のものをそのまま渡す。** 失敗時に -1 を
         * 返す箇所が残っており、musl からは EPERM に見える。まず通信が
         * できることを確かめる方を先にした (errno の整備は別件)。
         *
         * **riscv64 も同じディスパッチャを使う。** riscv64 は
         * kernel/net_socket.c をビルドしていないので、リンクを保つための
         * スタブを kernel/riscv64/net_socket.c に置いてある */
        case LINUX_SYS_SOCKET:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_socket((int)arch_syscall_arg0(frame),
                                                                         (int)arch_syscall_arg1(frame),
                                                                         (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_BIND:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_bind((int)arch_syscall_arg0(frame),
                                                                       (const void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                       (uint32_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_LISTEN:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_listen((int)arch_syscall_arg0(frame),
                                                                         (int)arch_syscall_arg1(frame)));
            return;
        /* accept4 の第 4 引数 (flags) は net_socket 側に渡せる口が無い。
         * **0 以外で来たら断る** —— 黙って無視すると、SOCK_NONBLOCK を
         * 期待した相手がブロックする fd を掴んで固まる */
        case LINUX_SYS_ACCEPT:
        case LINUX_SYS_ACCEPT4:
            if (syscall_no == LINUX_SYS_ACCEPT4 && (int)arch_syscall_arg3(frame) != 0) {
                arch_syscall_set_return(frame, (uint64_t)(int64_t)(-LINUX_EINVAL));
                return;
            }
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_accept((int)arch_syscall_arg0(frame),
                                                                         (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                         (uint32_t*)(uintptr_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_CONNECT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_connect((int)arch_syscall_arg0(frame),
                                                                          (const void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                          (uint32_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_GETSOCKNAME:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_getsockname((int)arch_syscall_arg0(frame),
                                                                              (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                              (uint32_t*)(uintptr_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_GETPEERNAME:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_getpeername((int)arch_syscall_arg0(frame),
                                                                              (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                              (uint32_t*)(uintptr_t)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_SENDMSG:
            arch_syscall_set_return(frame,
                                    (uint64_t)linux_bootstrap_sys_sendmsg((int)arch_syscall_arg0(frame),
                                                                          (struct linux_msghdr*)(uintptr_t)arch_syscall_arg1(frame),
                                                                          (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_RECVMSG:
            arch_syscall_set_return(frame,
                                    (uint64_t)linux_bootstrap_sys_recvmsg((int)arch_syscall_arg0(frame),
                                                                          (struct linux_msghdr*)(uintptr_t)arch_syscall_arg1(frame),
                                                                          (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_SENDTO:
            arch_syscall_set_return(frame,
                                    (uint64_t)net_socket_sendto((int)arch_syscall_arg0(frame),
                                                                (const void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                (size_t)arch_syscall_arg2(frame),
                                                                (int)arch_syscall_arg3(frame),
                                                                (const void*)(uintptr_t)arch_syscall_arg4(frame),
                                                                (uint32_t)arch_syscall_arg5(frame)));
            return;
        case LINUX_SYS_RECVFROM:
            arch_syscall_set_return(frame,
                                    (uint64_t)net_socket_recvfrom((int)arch_syscall_arg0(frame),
                                                                  (void*)(uintptr_t)arch_syscall_arg1(frame),
                                                                  (size_t)arch_syscall_arg2(frame),
                                                                  (int)arch_syscall_arg3(frame),
                                                                  (void*)(uintptr_t)arch_syscall_arg4(frame),
                                                                  (uint32_t*)(uintptr_t)arch_syscall_arg5(frame)));
            return;
        case LINUX_SYS_SETSOCKOPT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_setsockopt((int)arch_syscall_arg0(frame),
                                                                             (int)arch_syscall_arg1(frame),
                                                                             (int)arch_syscall_arg2(frame),
                                                                             (const void*)(uintptr_t)arch_syscall_arg3(frame),
                                                                             (uint32_t)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_GETSOCKOPT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_getsockopt((int)arch_syscall_arg0(frame),
                                                                             (int)arch_syscall_arg1(frame),
                                                                             (int)arch_syscall_arg2(frame),
                                                                             (void*)(uintptr_t)arch_syscall_arg3(frame),
                                                                             (uint32_t*)(uintptr_t)arch_syscall_arg4(frame)));
            return;
        case LINUX_SYS_SHUTDOWN:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)net_socket_shutdown((int)arch_syscall_arg0(frame),
                                                                           (int)arch_syscall_arg1(frame)));
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
                                    (uint64_t)(int64_t)sys_set_tid_address((int*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_FUTEX:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_futex((volatile int*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                    (int)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_NANOSLEEP:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_nanosleep(
                                        (const struct linux_timespec*)(uintptr_t)arch_syscall_arg0(frame),
                                        (struct linux_timespec*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_CLOCK_GETTIME:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_clock_gettime((int)arch_syscall_arg0(frame),
                                                                                            (struct linux_timespec*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_RT_SIGACTION:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_rt_sigaction((int)arch_syscall_arg0(frame),
                                                                                           (const struct linux_rt_sigaction_k*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                           (struct linux_rt_sigaction_k*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                           (size_t)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_RT_SIGPROCMASK:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_rt_sigprocmask((int)arch_syscall_arg0(frame),
                                                                                             (const uint64_t*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                             (uint64_t*)(uintptr_t)arch_syscall_arg2(frame),
                                                                                             (size_t)arch_syscall_arg3(frame)));
            return;
        case LINUX_SYS_GETRANDOM:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_getrandom((void*)(uintptr_t)arch_syscall_arg0(frame),
                                                                                        (size_t)arch_syscall_arg1(frame),
                                                                                        (unsigned)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_WRITEV:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_writev((int)arch_syscall_arg0(frame),
                                                                                    (const struct linux_iovec*)(uintptr_t)arch_syscall_arg1(frame),
                                                                                    (int)arch_syscall_arg2(frame)));
            return;
        case LINUX_SYS_READV:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_readv((int)arch_syscall_arg0(frame),
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
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_pipe2((int*)(uintptr_t)arch_syscall_arg0(frame),
                                                                 (int)arch_syscall_arg1(frame)));
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
                                    (uint64_t)(int64_t)sys_uname(
                                        (struct linux_utsname*)(uintptr_t)arch_syscall_arg0(frame)));
            return;
        case LINUX_SYS_GETRLIMIT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_getrlimit(
                                        (int)arch_syscall_arg0(frame),
                                        (struct linux_rlimit*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_SETRLIMIT:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_setrlimit(
                                        (int)arch_syscall_arg0(frame),
                                        (const struct linux_rlimit*)(uintptr_t)arch_syscall_arg1(frame)));
            return;
        case LINUX_SYS_PRLIMIT64:
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_prlimit64(
                                        (int)arch_syscall_arg0(frame),
                                        (int)arch_syscall_arg1(frame),
                                        (const struct linux_rlimit*)(uintptr_t)arch_syscall_arg2(frame),
                                        (struct linux_rlimit*)(uintptr_t)arch_syscall_arg3(frame)));
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
                                    (uint64_t)(int64_t)sys_sysinfo(
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
            /* **引数は 3 つ。**第 4 レジスタは flags ではない (kernel/sys_access.c) */
            arch_syscall_set_return(frame,
                                    (uint64_t)(int64_t)sys_faccessat((int)arch_syscall_arg0(frame),
                                                                     (const char*)(uintptr_t)arch_syscall_arg1(frame),
                                                                     (int)arch_syscall_arg2(frame), 0));
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
