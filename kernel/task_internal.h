#ifndef TASK_INTERNAL_H
#define TASK_INTERNAL_H

#include <stdint.h>
#include "task.h"
#include "arch_syscall.h"

#define TASK_TIMESLICE_TICKS 5

#define USER_STACK_PAGES       64
#define USER_STACK_GUARD_PAGES 1

/* **スタックは足りなくなったら伸ばす (task_grow_user_stack)。**
 *
 * 2026-08-30、GCC の `make all-host` が insn-attrtab.o で落ちた。cc1 が
 * `ESR=0x92000047` (下位 EL からのデータアボート、L3 の変換フォールト、
 * 書き込み)、`FAR=0x3ffffbfe60`、`SP0=0x3ffffbfec0`。
 * `sp_top - SP0 = 0x3f140` = 252KiB で、USER_STACK_PAGES 64 からガード 1 枚を
 * 引いた 63 ページとぴったり一致した。**ガードページを踏んでいた。**
 *
 * **最初から 8MiB 張ってはいけない。** alloc_user_stack は全ページを即座に
 * 張り、fork は aarch64_vm_clone_table で張ってある分を全部写す。同じ日の
 * 標本で vm_clone_table は既に 3% (21/736) を占めており、32 倍にすると
 * ここが支配的になる。**入口は 64 ページのまま、要るタスクだけ伸ばす。** */
#define USER_STACK_MAX_PAGES   2048    /* 上限 8MiB。Linux の既定と同じ */

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

int task_fork(arch_syscall_frame_t* frame);
int task_execve(arch_syscall_frame_t* frame, const char* path, char* const argv[], char* const envp[]);
void task_set_comm_from_path(struct task* t, const char* path);
uint64_t task_lock_irqsave(void);
void task_unlock_irqrestore(uint64_t flags);
void task_unlock_keep_irq(void);
int task_next_pid_locked(void);
struct task* task_alloc_struct(void);
int task_free_struct(struct task* t);
uint32_t task_choose_fork_cpu_locked(uint32_t fallback_cpu);
int task_mark_ready_on_cpu_locked_internal(struct task* t, uint32_t cpu_id);
uint32_t task_rebalance_ready_task_locked_internal(struct task* t);
struct cpu_local* task_this_cpu(void);
int task_wake_locked_internal(struct task* t);
uint32_t task_normalize_cpu_affinity_internal(uint32_t cpu_id);
struct task* task_runq_pop_locked_internal(struct cpu_local* cpu);
int task_is_idle_task_internal(struct task* t);
void task_refresh_cpu_local_msrs_internal(struct cpu_local* cpu);
void task_write_user_fs_base_internal(uint64_t fs_base);
#if ORTHOX_MEM_PROGRESS
void task_trace_progress_tick_internal(struct task* t, uint64_t now);
#endif

#endif
