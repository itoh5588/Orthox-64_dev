#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "fs.h"

struct elf_info;
struct orth_runq_stat;
struct wait_queue;

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_SLEEPING,
    TASK_IO_WAIT,
    TASK_ZOMBIE,
    TASK_DEAD
} task_state_t;

struct task;

#define ORTHOX_MAX_CPUS 64

struct cpu_local {
    uint64_t kernel_stack;
    uint64_t user_stack;
    struct cpu_local* self;
    uint32_t cpu_id;
    uint32_t reserved;
    struct task* current_task;
    struct task* idle_task;
    struct task* runq_head;
    struct task* runq_tail;
    volatile int resched_pending;
    uint32_t runq_count;
    uint32_t kernel_lock_depth;
};

struct task_context {
    uint64_t cr3, rip, rflags, reserved1; // 0, 8, 16, 24
    uint64_t cs, ss, fs, gs;             // 32, 40, 48, 56
    uint64_t rax, rbx, rcx, rdx, rdi, rsi, rsp, rbp; // 64, 72, 80, 88, 96, 104, 112, 120
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;   // 128, 136, 144, 152, 160, 168, 176, 184
    uint8_t fxsave_area[512] __attribute__((aligned(16))); // 192
} __attribute__((packed));

struct task {
    uint64_t kstack_top;
    uint64_t os_stack_ptr;
    struct task_context ctx; // ctx offset = 16
    int pid;
    int ppid;
    int pgid;
    int sid;
    uint64_t sig_pending;
    uint64_t sig_mask;
    uint64_t sig_handlers[32];
    uint64_t sig_action_masks[32];
    uint32_t sig_action_flags[32];
    int exit_status;
    task_state_t state;
    int cpu_affinity;
    uint64_t heap_break;
    uint64_t mmap_end;
    uint64_t user_entry;
    uint64_t user_stack;
    uint64_t user_stack_top;
    uint64_t user_stack_bottom;
    uint64_t user_stack_guard;
    uint64_t user_argc;
    uint64_t user_argv;
    uint64_t user_envp;
    uint64_t user_fs_base;
    uint64_t deferred_cr3;
    uint64_t tls_vaddr;
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;
    uint64_t sleep_until_ms;
    uint64_t trace_started_ms;
    uint64_t trace_last_ms;
    uint64_t trace_syscalls;
    uint64_t trace_brk_calls;
    uint64_t trace_mmap_calls;
    uint64_t trace_munmap_calls;
    uint64_t trace_mremap_calls;
    uint64_t trace_read_calls;
    uint64_t trace_write_calls;
    uint64_t trace_read_bytes;
    uint64_t trace_write_bytes;
    uint64_t trace_write_max;
    uint64_t trace_open_calls;
    uint64_t trace_close_calls;
    uint64_t trace_stat_calls;
    uint64_t trace_fstat_calls;
    uint64_t trace_lseek_calls;
    uint64_t trace_ioctl_calls;
    uint64_t trace_clock_calls;
    uint64_t trace_gettimeofday_calls;
    uint64_t trace_cow_faults;
    int timeslice_ticks;
    int trace_progress;
    char comm[64];
    char cwd[256];
    file_descriptor_t fds[MAX_FDS];
    struct task* next;
    struct task* runq_prev;
    struct task* runq_next;
    struct task* wait_next;
    struct wait_queue* wait_queue;
    uint8_t on_runq;
};

void task_init(void);
void task_set_cpu_count(uint32_t cpu_count);
uint32_t task_get_cpu_count(void);
int task_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count);
struct cpu_local* get_cpu_local_by_id(uint32_t cpu_id);
void task_bind_cpu_local(uint32_t cpu_id, struct task* current, struct task* idle,
                         uint64_t kernel_stack);
void task_install_cpu_local(uint32_t cpu_id);
struct task* task_create(uint64_t entry, uint64_t user_rsp);
struct task* task_create_on_cpu(uint64_t entry, uint64_t user_rsp, uint32_t cpu_id);
struct task* task_create_idle(uint32_t cpu_id);
int task_set_affinity(struct task* t, uint32_t cpu_id);
int task_mark_ready_on_cpu(struct task* t, uint32_t cpu_id);
int task_mark_sleeping(struct task* t);
int task_mark_io_wait(struct task* t);
int task_mark_io_wait_until(struct task* t, uint64_t deadline_ms);
int task_mark_zombie(struct task* t, int exit_status);
int task_wake(struct task* t);
int task_reap(struct task* t);
int task_set_fork_spread(int enabled);
int task_get_fork_spread(void);
void schedule(void);
struct cpu_local* get_cpu_local(void);
struct task* get_current_task(void);
void task_request_resched(void);
void task_request_resched_cpu(uint32_t cpu_id);
int task_consume_resched(void);
void task_on_timer_tick(void);
int task_prepare_initial_user_stack(uint64_t* pml4_virt, struct task* t,
                                    const struct elf_info* info,
                                    const struct elf_info* interp_info,
                                    char* const argv[], char* const envp[]);
void task_idle_loop(int poll_network);

#endif
