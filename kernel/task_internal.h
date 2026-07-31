#ifndef TASK_INTERNAL_H
#define TASK_INTERNAL_H

#include <stdint.h>
#include "task.h"
#include "arch_syscall.h"

#define TASK_TIMESLICE_TICKS 5

// Sv39 (riscv64) は 39bit VA のためユーザースタックを 2^38 未満に置く
#if defined(__riscv)
#define USER_STACK_TOP_VADDR   0x0000003FFFFFF000ULL
#define USER_MMAP_BASE_VADDR   0x0000002000000000ULL
#else
#define USER_STACK_TOP_VADDR   0x7FFFFFFFF000ULL
#define USER_MMAP_BASE_VADDR   0x4000000000ULL
#endif
#define USER_STACK_PAGES       64
#define USER_STACK_GUARD_PAGES 1

#define MSR_FS_BASE        0xC0000100
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

int task_fork(arch_syscall_frame_t* frame);
int task_execve(arch_syscall_frame_t* frame, const char* path, char* const argv[], char* const envp[]);
void task_set_comm_from_path(struct task* t, const char* path);
uint64_t task_lock_irqsave(void);
void task_unlock_irqrestore(uint64_t flags);
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
