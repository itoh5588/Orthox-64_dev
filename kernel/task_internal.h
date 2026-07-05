#ifndef TASK_INTERNAL_H
#define TASK_INTERNAL_H

#include <stdint.h>
#include "task.h"

#define TASK_TIMESLICE_TICKS 5

#define USER_STACK_TOP_VADDR   0x7FFFFFFFF000ULL
#define USER_STACK_PAGES       64
#define USER_STACK_GUARD_PAGES 1

#define MSR_FS_BASE        0xC0000100
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

int task_fork(struct syscall_frame* frame);
int task_execve(struct syscall_frame* frame, const char* path, char* const argv[], char* const envp[]);
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
struct task* task_runq_pop_locked_internal(struct cpu_local* cpu);
int task_is_idle_task_internal(struct task* t);
void task_refresh_cpu_local_msrs_internal(struct cpu_local* cpu);
void task_write_user_fs_base_internal(uint64_t fs_base);
#if ORTHOX_MEM_PROGRESS
void task_trace_progress_tick_internal(struct task* t, uint64_t now);
#endif

#endif
