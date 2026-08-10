#ifndef ORTHOX_LINUX_SYSCALL_H
#define ORTHOX_LINUX_SYSCALL_H

/*
 * Linux (asm-generic) システムコール層。**riscv64 と aarch64 で共有する。**
 *
 * もとは kernel/riscv64/syscall.c にあったが、実測するとアーキ固有の要素が
 * ほとんど無かった (インラインアセンブラ 0 / CSR 0 / ページテーブル操作 0)。
 * riscv64 と aarch64 は同じ asm-generic ABI なので、
 * **システムコール番号も struct のレイアウトも同一**。
 *
 * アーキごとに違うのは下の hook だけ。各アーキが実装する。
 */

#include <stdint.h>
#include "arch_syscall.h"
#include "fs.h"

struct arch_task_context;
struct task;

/* システムコール命令の次から再開させる。
 *
 *   riscv64   sepc は ecall 自身を指すので +4 が要る
 *   aarch64   **ELR_EL1 は svc の次を指しているので何もしない**
 *
 * ここを取り違えると、riscv64 は同じ ecall を無限に再実行し、
 * aarch64 は命令を 1 つ飛ばす (日報2026-08-09 追2-2 で実証済み) */
void arch_syscall_advance_pc(arch_syscall_frame_t* frame);

/* fork の子を「システムコールから戻った」形に整える */
void arch_syscall_set_user_return(arch_syscall_frame_t* frame, uint64_t pc, uint64_t sp,
                                  uint64_t ret, uint64_t arg1, uint64_t arg2);

/* いま走っているタスクの保存フレームへ書き戻す */
void arch_syscall_sync_current_user_frame(const arch_syscall_frame_t* frame);
void arch_syscall_set_current_context(struct arch_task_context* ctx);

/* コンソールのエコー設定 (termios) をアーキ側のドライバに聞く */
int arch_console_echo_enabled(void);

/* fd が持つ size の写しを inode から取り直す。fs の実装がアーキで違うため */
void arch_fs_refresh_size(file_descriptor_t* f);

/* uname(2) が返す文字列 */
const char* arch_uname_machine(void);
const char* arch_uname_version(void);

/* ユーザーのページテーブルを書き替えた後の TLB 破棄 (mmap / munmap / brk) */
void arch_syscall_flush_tlb(void);

/* コンソール入力。**アーキのドライバが持つ** (riscv64 は UART 受信割り込み、
 * aarch64 は PL011 の受信をまだ入れていない) */
int  arch_console_has_input(void);
int  arch_console_set_waiter(struct task* t);
void arch_console_clear_waiter(struct task* t);

/* 保存フレームへ書き戻す (exec / fork の後始末) */
void arch_task_store_user_frame_hook(struct arch_task_context* ctx,
                                     const arch_syscall_frame_t* frame);

/* もう進めないときに止まる */
void arch_halt_forever(void) __attribute__((noreturn));

/* puts は stdio.h。puthex はアーキの runtime.c が出す */
void puthex(uint64_t value);

/* 入口。アーキの例外ハンドラから呼ぶ */
void linux_syscall_dispatch(arch_syscall_frame_t* frame);

#endif
