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
