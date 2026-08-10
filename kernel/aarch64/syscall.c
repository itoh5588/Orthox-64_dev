/*
 * aarch64 のシステムコール入口 (薄い層)。
 *
 * 中身は kernel/linux_syscall.c (riscv64 と共有)。AArch64 と riscv64 は
 * 同じ asm-generic ABI なので、**番号も struct のレイアウトも同一**。
 * ここに置くのは、共有層が呼ぶ hook の aarch64 実装だけ。
 */
#include <stdint.h>
#include "fs.h"
#include "linux_syscall.h"
#include "aarch64/boot.h"
#include "aarch64/task.h"
#include "aarch64/trap.h"
#include "aarch64/vm.h"
#include "task.h"

/* **何もしない。** ELR_EL1 は svc の次の命令を指している。
 * riscv64 の sepc は ecall 自身を指すので +4 が要る、という違い
 * (日報2026-08-09 追2-2 で実測して確かめた) */
void arch_syscall_advance_pc(arch_syscall_frame_t* frame) {
    (void)frame;
}

/* fork の子を「システムコールから戻った」形に整える */
void arch_syscall_set_user_return(arch_syscall_frame_t* frame, uint64_t pc, uint64_t sp,
                                  uint64_t ret, uint64_t arg1, uint64_t arg2) {
    if (!frame) return;
    frame->elr = pc;
    frame->sp_el0 = sp;
    frame->x[0] = ret;
    frame->x[1] = arg1;
    frame->x[2] = arg2;
}

/* fd が持つ size の写しの取り直し。**aarch64 は共有の kernel/fs.c を
 * 使っており、あちらが inode から直接読む**ので何もしなくてよい
 * (riscv64 は自前の fs.c が写しを持っているため必要) */
void arch_fs_refresh_size(file_descriptor_t* f) {
    (void)f;
}

const char* arch_uname_machine(void) { return "aarch64"; }
const char* arch_uname_version(void) { return "Orthox-64 aarch64"; }

/* ユーザーのページテーブルを書き替えた後。**TTBR0 側だけで足りる**が、
 * ASID を使っていないので全部捨てている (日報の未実施表) */
void arch_syscall_flush_tlb(void) {
    __asm__ volatile("dsb ishst");
    __asm__ volatile("tlbi vmalle1is");
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

/* コンソール入力は kernel/aarch64/console.c に移した (P3)。
 * PL011 の受信割り込みが入ったので、ここのダミーは不要になった */

void arch_task_store_user_frame_hook(struct arch_task_context* ctx,
                                     const arch_syscall_frame_t* frame) {
    aarch64_task_store_user_frame(ctx, frame);
}

void arch_halt_forever(void) {
    aarch64_wait_forever();
    for (;;) { }
}

/* ---- sys_sleep_ms ---------------------------------------------------------
 *
 * kernel/sys_time.c は rdtsc を直書きしていて aarch64 では通らない
 * (`invalid input constraint 'a'`)。ここに実装する。
 *
 * **スピンで待たない。** 期限付きで寝て、タイマに起こしてもらう
 * (共有スケジューラの task_poll_sleep_wakeups が拾う)。
 * riscv64 も 07-31 にスピン待ちを廃止している (`c3ec630`) */
uint64_t arch_time_now_ms(void);
void kernel_yield(void);

int64_t sys_sleep_ms(uint64_t ms) {
    struct task* cur = get_current_task();
    uint64_t deadline;
    if (!cur) return -1;
    if (ms == 0) { kernel_yield(); return 0; }
    deadline = arch_time_now_ms() + ms;
    while (arch_time_now_ms() < deadline) {
        task_mark_io_wait_until(cur, deadline);
        kernel_yield();
    }
    return 0;
}

/* ---- 端末のフォアグラウンドプロセスグループ ------------------------------
 *
 * kernel/sys_proc.c も x86 の MSR 操作を直書きしていて通らない。
 * 必要なのは tcgetpgrp / tcsetpgrp の 2 本だけなのでここに置く。
 * **ジョブ制御はまだ無い**ので、値を覚えるだけ (ash は取得できれば動く) */
static int g_tty_pgrp;

int sys_tcgetpgrp(int fd) {
    (void)fd;
    if (g_tty_pgrp == 0) {
        struct task* cur = get_current_task();
        g_tty_pgrp = cur ? cur->pgid : 1;
    }
    return g_tty_pgrp;
}

int sys_tcsetpgrp(int fd, int pgrp) {
    (void)fd;
    if (pgrp <= 0) return -22;   /* EINVAL */
    g_tty_pgrp = pgrp;
    return 0;
}
