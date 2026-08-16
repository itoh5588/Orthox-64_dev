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

void aarch64_pmm_report_free(const char* label);

void arch_halt_forever(void) {
    /* **最初のユーザープロセスが exit した所。** ここで物理ページの残りを
     * 出す (P3-4)。fork した子の空間を返せていないと、fork/exec を繰り返す
     * ほど減っていく — **落ちないので、数えないと気づけない。**
     *
     * 共有の linux_syscall.c ではなくここに置くのは、riscv64 の出力を
     * 変えないため */
    aarch64_pmm_report_free("  pmm 残り  : ");
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

/* ---- 私物 syscall (ORTH_SYS_*) -------------------------------------------
 *
 * **画面とキーのように「その機械にしか無いもの」をここで受ける。**
 * 共有の linux_syscall.c は弱いシンボルで「何も扱えない」を既定にしており、
 * aarch64 だけがこれを上書きする (riscv64 は従来どおり ENOSYS)。
 *
 * x86 は kernel/syscall.c で同じ番号を扱っているが、あちらは Limine が
 * 用意した framebuffer を前提にしていて共有できない。**番号と struct の
 * 形だけを揃えてある**ので、ユーザー側 (DOOM) は同じコードで動く。
 */
#include "aarch64/fb.h"
#include "syscall.h"
#include "vmm.h"

uint64_t aarch64_vm_map_fb_user(arch_address_space_t as, uint64_t uva);
uint64_t arch_time_now_ms(void);

/* ユーザーに見せるフレームバッファの VA。
 *
 * **ユーザーの text (0x400000) / heap / stack と離す。** DOOM の musl は
 * mmap をここまで伸ばさないので衝突しない。固定にしているのは、
 * 2 回呼ばれても同じ番地を返すため (DOOM は 1 回しか呼ばないが、
 * 張り直しで別の番地を返すと古いポインタが生き残る) */
#define AARCH64_FB_USER_VA 0x0000000060000000ULL

int arch_orth_syscall(arch_syscall_frame_t* frame, uint64_t number) {
    switch (number) {
        case ORTH_SYS_GET_VIDEO_INFO: {
            struct video_info* info =
                (struct video_info*)(uintptr_t)arch_syscall_arg0(frame);
            const aarch64_fb_info_t* fb = aarch64_fb_info();
            if (!info || !fb || fb->base == 0) {
                arch_syscall_set_return(frame, (uint64_t)-1);
                return 1;
            }
            info->width  = fb->width;
            info->height = fb->height;
            info->pitch  = fb->pitch;
            info->bpp    = fb->depth;
            arch_syscall_set_return(frame, 0);
            return 1;
        }
        case ORTH_SYS_MAP_FRAMEBUFFER: {
            struct task* cur = get_current_task();
            uint64_t va;
            /* **いまのユーザー空間は ctx.root_pa (TTBR0)。**
             * カーネルスレッドなら 0 で、そこには張れない */
            if (!cur || cur->ctx.root_pa == 0) {
                arch_syscall_set_return(frame, 0);
                return 1;
            }
            va = aarch64_vm_map_fb_user(cur->ctx.root_pa, AARCH64_FB_USER_VA);
            /* **張ったら TLB を捨てる。** 直前まで未マップだった番地なので、
             * 古い「無い」という記憶が残っていると最初の書き込みで落ちる */
            if (va) arch_syscall_flush_tlb();
            arch_syscall_set_return(frame, va);
            return 1;
        }
        case ORTH_SYS_GET_TICKS_MS:
            arch_syscall_set_return(frame, arch_time_now_ms());
            return 1;
        case ORTH_SYS_SLEEP_MS:
            arch_syscall_set_return(frame,
                (uint64_t)sys_sleep_ms(arch_syscall_arg0(frame)));
            return 1;
        case ORTH_SYS_GET_KEY_EVENT: {
            /* **まだキーの経路が無い。** 空を返す (DOOM はデモを流す)。
             * シリアルのキーを流すのは次の段 */
            arch_syscall_set_return(frame, 0);
            return 1;
        }
        default:
            return 0;
    }
}
