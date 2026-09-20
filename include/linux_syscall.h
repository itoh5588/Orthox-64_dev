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
int arch_console_onlcr_enabled(void);

/* fd が持つ size の写しを inode から取り直す。fs の実装がアーキで違うため */
void arch_fs_refresh_size(file_descriptor_t* f);

/* ---- 乱数。**2 つ在って方針が違う** ------------------------------------
 *
 *   arch_random_bytes  **機械の乱数源だけ。**無ければ -1 を返す
 *                      (aarch64 = BCM2711 RNG200 / virtio-rng、riscv64 は
 *                      未実装なので linux_syscall.c の弱いシンボルが -1)。
 *                      **足りない分を作らない** —— 呼び手 (getrandom) は
 *                      ENOSYS を返す。乱数を得たつもりで先へ進ませないため
 *
 *   arch_random_fill   **必ず len バイト埋める。**乱数源が無ければ機械依存の
 *                      材料 (x86 は rdtsc / lapic / 現タスク) で混ぜて作る
 *
 * **2026-09-08 に arch_random_bytes を正とした。**3 アーキとも getrandom(2) は
 * こちらを使い、乱数源が無ければ ENOSYS を返す —— 混ぜ物で長さだけ揃えると、
 * 呼んだ側は乱数を得たつもりで先へ進むため。x86 も RDRAND だけを見る
 * (kernel/x86_64/rng.c)。
 *
 * **2026-09-09、口によって使い分けることにした。**getrandom(2) が ENOSYS を
 * 返せるのは、呼び手がそれを見て別の手に移れるからで、実際 CPython は
 * /dev/urandom へ退避する。**ところが /dev/urandom も同じ arch_random_bytes を
 * 見ていたので退避先が塞がっており、RDRAND の無い x86 (-cpu qemu64) と
 * riscv64 では python3 が起動しなかった** (日報2026-09-09 で実測)。
 *
 *   getrandom(2)             arch_random_bytes  源が無ければ ENOSYS
 *   /dev/random /dev/urandom arch_random_fill   必ず埋める
 *
 * **Linux の /dev/urandom は読み手にエラーを返さない口**なので、この形が
 * 元の契約に合う。源の無い機械では中身の質が落ちる —— それは
 * arch_random_fill の側が起動時に 1 度警告する */
int64_t arch_random_bytes(void* buf, size_t len);
void    arch_random_fill(void* buf, size_t len);

/* uname(2) が返す文字列 */
const char* arch_uname_machine(void);

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

/* 機械をリセットする (reboot(2) の受け皿)。
 *
 * 戻り値: 0 = リセットを仕掛けた (通常ここから戻らない)
 *        -1 = この機械では出来ない → 呼び出し元は -ENOSYS を返す
 *
 * aarch64 は BCM2711 の watchdog (Pi 4 に PSCI は無い)。
 * riscv64 / x86_64 はまだ手段を持たない */
int arch_system_reset(void);

/* puts は stdio.h。puthex はアーキの runtime.c が出す */
void puthex(uint64_t value);

/* 入口。アーキの例外ハンドラから呼ぶ */
void linux_syscall_dispatch(arch_syscall_frame_t* frame);

#endif
