/*
 * riscv64 のシステムコール入口 (薄い層)。
 *
 * **中身は kernel/linux_syscall.c に移した。** 実測でアーキ固有の要素が
 * ほとんど無く、aarch64 と同じ asm-generic ABI だったため。
 * ここに残るのは、共有層が呼ぶ hook の riscv64 実装だけ。
 */
#include <stdint.h>
#include "fs.h"
#include "linux_syscall.h"
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/syscall.h"
#include "riscv64/trap.h"
#include "task.h"

extern void riscv64_fs_refresh_xv6fs_size(file_descriptor_t* f);

/* **sepc は ecall 自身を指している。** 進めないと同じ ecall を無限に再実行する
 * (aarch64 の ELR_EL1 は svc の次を指すので、あちらは何もしない) */
void arch_syscall_advance_pc(arch_syscall_frame_t* frame) {
    if (frame) frame->sepc += 4;
}

void arch_syscall_set_user_return(arch_syscall_frame_t* frame, uint64_t pc, uint64_t sp,
                                  uint64_t ret, uint64_t arg1, uint64_t arg2) {
    riscv64_trap_set_user_return(frame, pc, sp, ret, arg1, arg2);
}

void arch_fs_refresh_size(file_descriptor_t* f) {
    riscv64_fs_refresh_xv6fs_size(f);
}

const char* arch_uname_machine(void) { return "riscv64"; }
const char* arch_uname_version(void) { return "Orthox-64 riscv64"; }

/* 例外ハンドラ (trap.c) から呼ばれる名前。共有層へ渡すだけ */
void riscv64_syscall_dispatch(arch_syscall_frame_t* frame) {
    linux_syscall_dispatch(frame);
}

/* trap.c が呼ぶ名前。共有層の実体へ渡すだけ */
void riscv64_syscall_set_current_context(struct arch_task_context* ctx) {
    arch_syscall_set_current_context(ctx);
}

/* 共有層は arch_console_echo_enabled を自分で定義している (termios を
 * 持っているのがあちらのため)。ここで重ねない */

/* ---- 共有層が要求する残りの hook -------------------------------------- */

int  riscv64_console_has_input(void);
int  riscv64_console_set_waiter(struct task* t);
void riscv64_console_clear_waiter(struct task* t);
void riscv64_wait_forever(void);
void riscv64_task_store_user_frame(struct arch_task_context* ctx,
                                   const arch_task_exec_frame_t* frame);

void arch_syscall_flush_tlb(void) { riscv64_sfence_vma(); }

int  arch_console_has_input(void)              { return riscv64_console_has_input(); }
int  arch_console_set_waiter(struct task* t)   { return riscv64_console_set_waiter(t); }
void arch_console_clear_waiter(struct task* t) { riscv64_console_clear_waiter(t); }

void arch_task_store_user_frame_hook(struct arch_task_context* ctx,
                                     const arch_syscall_frame_t* frame) {
    riscv64_task_store_user_frame(ctx, frame);
}

void arch_halt_forever(void) { riscv64_wait_forever(); for (;;) { } }

/* trap.c が呼ぶ名前。共有層の実体へ渡すだけ */
void riscv64_syscall_sync_current_user_frame(const arch_syscall_frame_t* frame) {
    arch_syscall_sync_current_user_frame(frame);
}

/* 共有層が定義している echo 判定を、riscv64 の名前でも見えるようにする
 * (kernel/riscv64/fs.c などが呼んでいる) */
int riscv64_console_echo_enabled(void) { return arch_console_echo_enabled(); }
