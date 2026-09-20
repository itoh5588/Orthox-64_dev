#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "fs.h"
#include "arch_task.h"
#include "arch_syscall.h"
#include "arch_vm.h"
#include "arch_time.h"

/* ---- ユーザー空間の番地の割り当て ---------------------------------------
 *
 * **共有層から見える所に置く (2026-09-10)。**もとは kernel/task_internal.h に
 * あったが、あちらは共有層から見えない (linux_syscall.c:318 に同じ理由の
 * 記述がある)。そのため kernel/linux_syscall.c の mmap は上限を関数の中に
 * 直書きしていた。**値はどのアーキも今までと同じ。**
 */
// Sv39 (riscv64) / 4KB granule + T0SZ=25 (aarch64) は **どちらも 39bit VA**
// なので、ユーザースタックを 2^38 未満に置く。
//
// **aarch64 をここに入れ忘れると、x86 と同じ 2^47 を要求して落ちる。**
// P2 (musl) で実際に踏んだ:
//   ESR=0x92000004 (下位 EL のデータアボート, translation fault level 0)
//   FAR=0x00007fffffffef00   ELR=crt0 の `ldr x1, [x9]`
// P1 の hello が通っていたのは、あれが sp を一度も触らなかったため。
// **「ユーザープロセスが動いた」はスタックが張れている証拠にならない。**
//
// **上限もここに置く (2026-09-10)。**kernel/linux_syscall.c の mmap が
// Sv39 の値 (0x3F00000000) を関数の中に直書きしており、x86 と共通化する
// ときに持ち込めなかった。値はどのアーキも今までと同じ。
#if defined(__riscv) || defined(__aarch64__)
#define USER_STACK_TOP_VADDR   0x0000003FFFFFF000ULL
#define USER_MMAP_BASE_VADDR   0x0000002000000000ULL
#define USER_MMAP_TOP_VADDR    0x0000003F00000000ULL
#else
#define USER_STACK_TOP_VADDR   0x7FFFFFFFF000ULL
/* **sys_vm.c の MMAP_BASE_ADDR に揃えた (2026-09-11)。**ここは
 * 0x4000000000 だったが、mmap の下端は kernel/x86_64/sys_vm.c の
 * MMAP_BASE_ADDR (0x200000000000) が本家で、find_mmap_gap が切り上げて
 * いたため **宣言と実際がずれていた。**mmap を共有層へ出すにあたって
 * 実際のほうに合わせる。mmap_end の初期値が変わるだけで、切り上げに
 * よって配置はこれまでと同じ */
#define USER_MMAP_BASE_VADDR   0x0000200000000000ULL
#define USER_MMAP_TOP_VADDR    0x00007F0000000000ULL
#endif

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
    /* 直前にこの CPU から降りたタスク。切り替えを終えた側 (次のタスク) が
     * task_finish_switch で on_cpu を落とす。訳は task.c の task_reap */
    struct task* switched_from;
};

/* task_context is now provided by the arch layer (arch_task_context). */
/* main-side code uses `struct task_context` and `task_context_t`; both alias to arch_task_context. */
typedef struct arch_task_context task_context_t;
#define task_context arch_task_context

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
    /* **umask はプロセスごとの状態で fork で引き継ぐ。** 既定は 022。
     * 0 で初期化されると「作ったファイルが誰でも書ける」になるので、
     * task を作る所で必ず 022 を入れること (task_set_default_umask) */
    uint32_t umask;
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
    /* **いまどこかの CPU の上にいる (切り替えの途中を含む)。**schedule が
     * 載せるときに立て、降りたタスクの切り替えが終わってから次のタスクの
     * 側で落とす (Linux の on_cpu)。立っている間は回収しない */
    volatile uint32_t on_cpu;
};

void task_init(void);
void task_set_cpu_count(uint32_t cpu_count);
uint32_t task_get_cpu_count(void);
int task_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count);
struct cpu_local* get_cpu_local_by_id(uint32_t cpu_id);
void task_finish_switch(void);
struct task* task_find_zombie_child(int parent_pid, int pid, int* found_child);
int task_signal_pid(int pid, int sig, int terminate, int exit_status, int parent_sig);
int task_set_pgid(int pid, int pgid);
int task_kill_pgrp(int pgid, int exclude_pid, int sig, int exit_status,
                   int* pids, int max);
struct task_snapshot {
    int pid;
    int ppid;
    task_state_t state;
    char comm[16];
};
int task_snapshot(struct task_snapshot* out, int max, int* more);
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
/* 寝ると決めた後、切り替わる前に条件が揃ったときの取り消し。
 * task_wake と違い runqueue には積まない (走行中のタスクを積まないため) */
int task_cancel_sleep(struct task* t);
int task_mark_zombie(struct task* t, int exit_status);
int task_wake(struct task* t);
int task_reap(struct task* t);
struct task* task_find_by_pid(int pid);
/* 親が死ぬときに子を始末する (kernel/task.c のコメント参照) */
void task_reap_orphans_of(int pid);

/* 子の終了を待つ親。**焼かずに寝かせる** (kernel/task.c のコメント参照)。
 * 待ち時間の上限は呼び手が決める —— 親を起こす経路は exit にしかないので、
 * kill や SIGSEGV で終わった子は時間切れの側で拾う */
#define TASK_CHILD_WAIT_POLL_MS 50
void task_child_exit_wake(void);
void task_wait_child_exit(int ppid, int want, uint64_t timeout_ms);
int task_set_fork_spread(int enabled);
int task_get_fork_spread(void);
void schedule(void);
struct cpu_local* get_cpu_local(void);
struct task* get_current_task(void);
task_context_t* task_current_context(void);
void task_request_resched(void);
void task_request_resched_cpu(uint32_t cpu_id);
int task_consume_resched(void);
void task_on_timer_tick(void);
/* 期限切れの sleep/IO 待ちを起こす。task_on_timer_tick から呼ばれるほか、
 * 割り込みに頼れない環境ではブロック待ちループ側からも直接呼ぶ */
int task_poll_sleep_wakeups(void);
/* スタックの下端へのフォルトを 1 ページずつ埋める。成功 0 / 断るなら -1。
 * 訳は kernel/task_internal.h の USER_STACK_MAX_PAGES に書いた */
int task_grow_user_stack(struct task* t, uint64_t fault_addr);

int task_prepare_initial_user_stack(arch_address_space_t address_space, struct task* t,
                                    const struct elf_info* info,
                                    const struct elf_info* interp_info,
                                    char* const argv[], char* const envp[]);
void task_idle_loop(int poll_network);

#endif
