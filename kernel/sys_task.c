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
#include "pmm.h"             /* PAGE_SIZE / pmm_get_*_pages */
#include "arch_time.h"       /* arch_time_now_ms */
#include "linux_syscalls.h"  /* struct linux_sysinfo */
#include "xv6fs.h"           /* xv6fs_now_sec */
#include "linux_abi.h"       /* LINUX_SIGCHLD */
#include "linux_syscall.h"   /* arch_halt_forever */
#include "stdio.h"           /* puts */
#include "sound.h"           /* sound_beep_stop */

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
/* **CPU を明け渡す。3 アーキ共通 (2026-09-20)。**
 * 以前は kernel/x86_64/sys_time.c にあり、aarch64 / riscv64 では
 * 番号が繋がっておらず ENOSYS だった (日報2026-09-19 §9-5)。
 * 中身は kernel_yield() だけで、アーキに依るものが無い */
int sys_sched_yield(void) {
    kernel_yield();
    return 0;
}

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

/* ---- sysinfo --------------------------------------------------------------
 * **x86 と linux 側で答える値が違っていた (2026-09-13 に畳んだ)。**
 *
 * | | x86 (旧) | linux 側 (採用) |
 * |---|---|---|
 * | totalram | limine の memmap の USABLE の合計 | pmm が管理するページ数 |
 * | freeram  | **totalram と同じ (常に全部空き)** | pmm の空きページ数 |
 * | mem_unit | 1 | PAGE_SIZE |
 * | uptime   | 起動からの秒 | **入れていなかった (0)** |
 * | 書く大きさ | 368 バイト (musl の __reserved まで) | 112 バイト (Linux の struct sysinfo) |
 *
 * **linux 側に寄せ、uptime だけ x86 から取った。**材料の memmap は x86 にしか
 * 無いが、pmm_get_*_pages と arch_time_now_ms は 3 アーキにある。
 *
 * **mem_unit を 0 にしないこと。**呼び手が totalram に掛けるので 0 だと
 * 「メモリ 0」に見え、確保をあきらめる側がいる。
 *
 * 書くのは 112 バイトだけ。musl の struct sysinfo は末尾に __reserved[256] を
 * 持つ 368 バイトなので越えないが、Linux カーネルの定義より長く書く理由は無い */
int sys_sysinfo(struct linux_sysinfo* info) {
    uint8_t* p;
    uint64_t freep;
    if (!info) return -LINUX_EFAULT;
    p = (uint8_t*)info;
    for (size_t i = 0; i < sizeof(*info); i++) p[i] = 0;
    freep = pmm_get_free_pages();
    info->uptime   = (int64_t)(arch_time_now_ms() / 1000ULL);
    info->mem_unit = PAGE_SIZE;
    info->totalram = pmm_get_allocated_pages() + freep;
    info->freeram  = freep;
    info->procs    = 1;
    return 0;
}

/* ---- getcwd ---------------------------------------------------------------
 * **linux 側は Linux の返り値の規約を守っていなかった (2026-09-13 に畳んだ)。**
 *
 * | | x86 (fs_getcwd) | linux 側 (その場で計算) |
 * |---|---|---|
 * | 成功        | NUL 込みの長さ | **buf のポインタ** |
 * | 足りない    | -ERANGE        | **0** |
 * | buf が NULL | -EFAULT        | **0** |
 * | cwd が空    | "" を返す      | "/" を返す |
 *
 * musl の getcwd() は「ret < 0 なら失敗、**ret == 0 なら ENOENT**」と読む
 * (ports/musl/src/unistd/getcwd.c)。linux 側はバッファ不足で 0 を返すので、
 * **ERANGE ではなく ENOENT になり、足りなければ広げて呼び直す側が諦める。**
 * 成功時のポインタは正の値なので偶然通っていた。
 *
 * **返り値は x86 (Linux の規約) に、空の cwd の救済は linux 側に寄せた。** */
int sys_getcwd(char* buf, size_t size) {
    struct task* current = get_current_task();
    const char* cwd = (current && current->cwd[0]) ? current->cwd : "/";
    size_t i = 0;
    if (!buf) return -LINUX_EFAULT;
    while (cwd[i] && i + 1 < size) {
        buf[i] = cwd[i];
        i++;
    }
    /* size が 0 のときもここに落ちる (1 文字も入らない) */
    if (cwd[i] != '\0' || i >= size) return -LINUX_ERANGE;
    buf[i] = '\0';
    return (int)(i + 1);
}

/* ---- nanosleep ------------------------------------------------------------
 * **x86 は期限前に戻りえた (2026-09-13 に畳んだ)。**
 *
 * | | x86 (sys_sleep_ms 経由) | linux 側 (採用) |
 * |---|---|---|
 * | 途中で起こされたとき | **そのまま戻る** | 期限を見て寝直す |
 * | 0 ミリ秒             | 眠りに入る      | yield だけ |
 * | 時計                 | lapic の tick   | arch_time_now_ms (x86 では同じ lapic) |
 *
 * 寝ている間に、期限と無関係な経路 (console 待ちの起床など) から READY に
 * されることがある。x86 は TASK_SLEEPING でなくなった時点で戻っていた。
 * 期限で起こすのは 3 アーキ共通の task_on_timer_tick -> task_poll_sleep_wakeups
 * (kernel/sched.c) なので、linux 側の作りがそのまま x86 でも動く。
 *
 * **musl の sleep() は nanosleep(&tv, &tv) と req と rem に同じポインタを
 * 渡す。**rem を先に書くと要求時間を自分で潰すので、必ず req を退避してから
 * 触ること (riscv64-sleep-smoke がこの呼び方で回している) */
int sys_nanosleep(const struct linux_timespec* req, struct linux_timespec* rem) {
    struct task* current = get_current_task();
    int64_t req_sec;
    int64_t req_nsec;
    uint64_t ms;
    uint64_t deadline;

    if (!req) return -LINUX_EFAULT;
    req_sec = req->tv_sec;
    req_nsec = req->tv_nsec;
    /* **時刻の指定が不正なら EINVAL。**Linux の規定 */
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
    while (arch_time_now_ms() < deadline) {
        task_mark_io_wait_until(current, deadline);
        kernel_yield();
    }
    return 0;
}

/* ---- clock_gettime --------------------------------------------------------
 * **REALTIME の時計の出どころが x86 と linux 側で違っていた (2026-09-13 に畳んだ)。**
 *
 * | | x86 | linux 側 |
 * |---|---|---|
 * | REALTIME の秒  | CMOS の RTC | SNTP で合っていればそれ、無ければ xv6fs の通し番号 |
 * | REALTIME の nsec | **常に 0** | ミリ秒から |
 * | MONOTONIC      | lapic の tick | arch_time_now_ms (x86 では同じ lapic) |
 *
 * **3 つを順に見る形にした: SNTP -> RTC -> xv6fs の通し番号。**
 *
 *   - SNTP が一番正しい (aarch64 は起動時に合わせる。riscv64 には網が無い)
 *   - RTC は x86 にだけある。SNTP が合っていないときの退き先として、
 *     通し番号より良い
 *   - **xv6fs_now_sec() は壁時計ではない。**xv6fs.c 自身が「単調に増える通し
 *     番号を秒の形で持っているだけ」と書いており、マウントのたびに 86400 秒
 *     進む。実機では実時刻より +39 日進み、起動ごとに +1 日離れていた
 *     (2026-09-05 実測)。TLS が証明書の有効期限をこれで見るので、最後の
 *     手段にとどめる (ファイルの前後関係だけは保たれる)
 *
 * aarch64 / riscv64 は RTC が 0 なので答えは変わらない。x86 は SNTP で
 * 合っていればそちらを使い、nsec が入るようになる */

/* SNTP で合わせた壁時計 (Unix 秒)。まだなら 0。**riscv64 には lwIP が無い**ので
 * 弱いシンボルで「無い」を既定にする (x86 / aarch64 は kernel/lwip_port.c の
 * 強いものが選ばれる) */
uint32_t lwip_port_wallclock_sec(void);
__attribute__((weak)) uint32_t lwip_port_wallclock_sec(void) { return 0; }

/* RTC を持たない機械の既定 (include/arch_time.h) */
__attribute__((weak)) uint64_t arch_rtc_seconds(void) { return 0; }

int sys_clock_gettime(int clock_id, struct linux_timespec* ts) {
    uint64_t ms;
    if (!ts) return -LINUX_EFAULT;
    if (clock_id != 0 && clock_id != 1) return -LINUX_EINVAL;
    ms = arch_time_now_ms();
    ts->tv_nsec = (int64_t)((ms % 1000ULL) * 1000000ULL);
    if (clock_id == 0) {
        uint64_t sec = lwip_port_wallclock_sec();
        if (sec == 0) sec = arch_rtc_seconds();
        if (sec == 0) sec = xv6fs_now_sec();
        ts->tv_sec = (int64_t)sec;
    } else {
        ts->tv_sec = (int64_t)(ms / 1000ULL);
    }
    return 0;
}

/* ---- exit / exit_group / wait4 --------------------------------------------
 * **別実装 29 組のうちの 2 組を畳んだもの (2026-09-13)。**もとは
 *
 *     x86              kernel/sys_proc.c      の sys_exit / sys_wait4
 *     aarch64/riscv64  kernel/linux_syscall.c の linux_bootstrap_sys_exit /
 *                                                linux_bootstrap_sys_wait4
 *
 * の 2 つずつがあり、**どちらも他方に無い直しを持っていた。**
 *
 * exit:
 * | | x86 (旧) | linux 側 |
 * |---|---|---|
 * | 親が居ない (ppid==0、起動直後のタスク) | **無限に zombie loop** し続ける | arch_halt_forever() で止める |
 * | ビープ音   | sound_beep_stop() で止める | **止めていなかった** (DOOM がクラッシュ音を鳴らし続けうる) |
 * | SIGCHLD が SIG_IGN のとき | pending を立てない | **無条件に立てる** |
 *
 * 3 つとも「x86 だけが正しい」でも「linux 側だけが正しい」でもなく、
 * **両方の直しを合わせた。**bootstrap task (kernel が直接起こした最初の
 * ユーザータスク。ppid は 0 で親が存在しない) が exit したときに x86 だけが
 * 無限ループに陥っていたのは実害のある方の抜けなので、linux 側の
 * arch_halt_forever() を採った。
 *
 * wait4:
 * | | x86 (旧) | linux 側 (採用) |
 * |---|---|---|
 * | zombie を reap したとき | **自分の sig_pending の SIGCHLD ビットを落とす** | 落とさない |
 *
 * 子が複数いて 1 匹だけ reap したとき、x86 は SIGCHLD の pending ビットを
 * 消してしまう。ビットは 1 本しかない (子ごとに個別の合図ではない) ので、
 * まだ zombie の残りが居ても消える。signal ハンドラで待つ側 (wait4 を
 * 呼ばない経路) がそこで気づき損ねる。linux 側は消さないので、そちらに
 * 揃えた。busybox ash の待ち方 (wait4 を ECHILD まで回す) はどちらでも
 * 変わらない。
 *
 * find_task_by_pid_locked (kernel/sys_proc.c) は BKL を要求して警告するが、
 * aarch64 / riscv64 の syscall 入口は BKL を握らない (kernel/task.c の
 * task_find_by_pid のコメント参照)。exit の親探しは 3 アーキ共通で通る道
 * なので、ロック無しの task_find_by_pid を使う。 */

extern struct task* task_list;

/* 音源を持たない機械 (riscv64) の既定。x86 / aarch64 は kernel の各 arch 配下の
 * sound.c が持つ実物が強いシンボルとして勝つ (include/sound.h に素の宣言がある) */
__attribute__((weak)) void sound_beep_stop(void) { }

void sys_exit(int status) {
    struct task* current = get_current_task();

    /* **親が居ないタスクの exit。**カーネルが直接起こした最初のユーザー
     * タスク (bootstrap) にはこれ以上進める先が無い。x86 はここが抜けて
     * おり、zombie のまま kernel_yield() を無限に回し続けていた */
    if (!current || current->ppid == 0) {
        puts("  bootstrap user exit\n");
        arch_halt_forever();
    }

    sound_beep_stop();

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (current->fds[fd].in_use) {
            /* **sys_close であって fs_close ではない。**riscv64 は
             * kernel/riscv64/fs.c に fs_close を持たず sys_close だけを
             * 出している。3 アーキで共通なのは sys_close の方 */
            (void)sys_close(fd);
        }
    }

    /* **自分の子を始末してから zombie になる。**入っていなかったので、
     * 親のいない zombie が溜まっていた (2026-09-08 に実測) */
    task_reap_orphans_of(current->pid);
    task_mark_zombie(current, status);

    {
        struct task* parent = task_find_by_pid(current->ppid);
        if (parent) {
            /* SIGCHLD が SIG_IGN (ハンドラ値 1) なら pending を立てない。
             * kernel/sys_signal.c の SIG_IGN 判定と同じ規約 */
            if (parent->sig_handlers[LINUX_SIGCHLD] != 1ULL) {
                parent->sig_pending |= (1ULL << LINUX_SIGCHLD);
            }
            /* **IO_WAIT も起こす (2026-09-19)。**pselect6 / ppoll は
             * task_mark_io_wait で寝るので、SLEEPING だけ見ていると
             * SIGCHLD を立てても起きない。make -j は子の終わりを pselect の
             * EINTR で知るので、-j の枠を使い切ったところで永久に止まった
             * (QEMU で task_list を gdb で読み、make が IO_WAIT のまま
             * sig_pending に SIGCHLD を持っているのを確認)。起こされた側は
             * どれも条件を見直して寝直すので、余計に起こしても害は無い */
            if (parent->state == TASK_SLEEPING || parent->state == TASK_IO_WAIT) {
                task_wake(parent);
            }
        }
    }

    /* **待ち行列で寝ている親を起こす。**これが無いと親は時間切れ
     * (TASK_CHILD_WAIT_POLL_MS) まで気づかない */
    task_child_exit_wake();
    while (1) kernel_yield();
}

/* aarch64 の例外ハンドラから (EL0 のフォールトで殺すとき) 呼ばれる名前。
 * 元は linux_syscall.c の linux_bootstrap_sys_exit を直に指していた */
void linux_task_kill_current(int status) {
    sys_exit(status);
}

int64_t sys_wait4(int pid, int* wstatus, int options) {
    struct task* current = get_current_task();
    if (!current) return -LINUX_ESRCH;
    while (1) {
        int found_child = 0;
        /* 探すのはロックの中 (task_find_zombie_child)。wstatus への書き込み
         * (CoW のフォルトが起きうる) と回収はロックの外 */
        struct task* zombie = task_find_zombie_child(current->pid, pid, &found_child);
        if (zombie) {
            int child_pid = zombie->pid;
            if (wstatus) *wstatus = zombie->exit_status << 8;
            (void)task_reap(zombie);
            return child_pid;
        }
        if (!found_child) return -LINUX_ECHILD;
        /* WNOHANG: 生きている子はいるがゾンビ無し -> ブロックせず 0。
         * これを無視すると make -j の非ブロッキング reap が子の終了まで
         * 眠り、並列ジョブ投入が完全に直列化する (実測で確認) */
        if (options & LINUX_WNOHANG) return 0;
        /* **焼かずに寝る。**子の exit で起こされるか、遅くとも時間切れで
         * 自力で起きる */
        task_wait_child_exit(current->pid, pid, TASK_CHILD_WAIT_POLL_MS);
    }
}
