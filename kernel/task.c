#include <stdint.h>
#include <stddef.h>
#include "kassert.h"
#include "task.h"
#include "task_internal.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "net_socket.h"
#include "net.h"
#include "smp.h"
#include "spinlock.h"
#include "wait.h"       /* 子の終了を待つ親を焼かずに寝かせる */

static struct cpu_local g_cpu_locals[ORTHOX_MAX_CPUS];
struct task* task_list = NULL;
static int next_pid = 1;
static uint32_t g_cpu_count = 1;
static spinlock_t g_task_lock;

extern void puts(const char* s);
extern void puthex(uint64_t v);

#ifndef ORTHOX_MEM_TRACE
#define ORTHOX_MEM_TRACE 0
#endif

#ifndef ORTHOX_MEM_PROGRESS
#define ORTHOX_MEM_PROGRESS 0
#endif

static int free_task_struct(struct task* t);
static int task_is_idle_task(struct task* t);
static int task_can_migrate_locked(struct task* t);
static int normalize_cpu_affinity(uint32_t cpu_id);
static void task_runq_push_locked(struct task* t, uint32_t cpu_id);

static void task_assert_stack_aligned(uint64_t kstack_top) {
    if (!kstack_top) return;
    KASSERT((kstack_top & (PAGE_SIZE - 1)) == 0);
}

static void task_assert_address_space_owned(struct task* t) {
    if (!t || task_is_idle_task(t)) return;
    uint64_t _as = arch_task_context_get_address_space(&t->ctx);
    KASSERT(_as != 0);
    KASSERT((_as & (PAGE_SIZE - 1)) == 0);
}

void task_set_comm_from_path(struct task* t, const char* path) {
    const char* base = path;
    if (!t) return;
    if (!base) base = "";
    for (const char* p = path; p && *p; p++) {
        if (*p == '/') base = p + 1;
    }
    for (int i = 0; i < 63; i++) {
        t->comm[i] = base[i];
        if (base[i] == '\0') return;
    }
    t->comm[63] = '\0';
}
uint64_t task_lock_irqsave(void) {
    return spin_lock_irqsave(&g_task_lock);
}

void task_unlock_irqrestore(uint64_t flags) {
    spin_unlock_irqrestore(&g_task_lock, flags);
}

/* **ロックだけ放して、割り込みは開けない。**schedule() が切り替えの区間で使う。
 * 訳は kernel/sched.c の arch_context_switch の手前のコメント */
void task_unlock_keep_irq(void) {
    spin_unlock(&g_task_lock);
}

int task_next_pid_locked(void) {
    return next_pid++;
}

#if ORTHOX_MEM_PROGRESS
void task_trace_progress_tick_internal(struct task* t, uint64_t now) {
    if (!t || !t->trace_progress) return;
    if (t->trace_last_ms != 0 && now - t->trace_last_ms < 5000) return;
    t->trace_last_ms = now;

    puts("[memprogress] pid=0x"); puthex((uint64_t)t->pid);
    puts(" ms=0x"); puthex(now - t->trace_started_ms);
    puts(" sys=0x"); puthex(t->trace_syscalls);
    puts(" brk=0x"); puthex(t->trace_brk_calls);
    puts(" mmap=0x"); puthex(t->trace_mmap_calls);
    puts(" munmap=0x"); puthex(t->trace_munmap_calls);
    puts(" mremap=0x"); puthex(t->trace_mremap_calls);
    puts(" rd=0x"); puthex(t->trace_read_calls);
    puts(" wr=0x"); puthex(t->trace_write_calls);
    puts(" rdb=0x"); puthex(t->trace_read_bytes);
    puts(" wrb=0x"); puthex(t->trace_write_bytes);
    puts(" wrmax=0x"); puthex(t->trace_write_max);
    puts(" op=0x"); puthex(t->trace_open_calls);
    puts(" cl=0x"); puthex(t->trace_close_calls);
    puts(" st=0x"); puthex(t->trace_stat_calls);
    puts(" fst=0x"); puthex(t->trace_fstat_calls);
    puts(" seek=0x"); puthex(t->trace_lseek_calls);
    puts(" ioctl=0x"); puthex(t->trace_ioctl_calls);
    puts(" clk=0x"); puthex(t->trace_clock_calls);
    puts(" gtod=0x"); puthex(t->trace_gettimeofday_calls);
    puts(" cow=0x"); puthex(t->trace_cow_faults);
    puts(" heap=0x"); puthex(t->heap_break);
    puts(" mmap_end=0x"); puthex(t->mmap_end);
    puts(" pmm_alloc=0x"); puthex(pmm_get_allocated_pages());
    puts(" pmm_free=0x"); puthex(pmm_get_free_pages());
    puts("\r\n");
}
#endif

enum task_migration_reason {
    TASK_MIGRATE_OK = 0,
    TASK_MIGRATE_BLOCK_NULL,
    TASK_MIGRATE_BLOCK_IDLE,
    TASK_MIGRATE_BLOCK_NOT_READY,
    TASK_MIGRATE_BLOCK_NOT_ON_RUNQ,
};

static enum task_migration_reason task_migration_reason_locked(struct task* t);

static void idle_task_entry(void) {
    task_finish_switch();
    task_idle_loop(0);
}

static void task_init_idle_context(struct task* t) {
    uint64_t* sp;
    if (!t || !task_is_idle_task(t)) return;
    sp = (uint64_t*)(t->kstack_top - 8);
    *sp = (uint64_t)idle_task_entry;
    *(--sp) = 0x202;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    arch_task_context_init_kernel_entry(&t->ctx, (uint64_t)idle_task_entry, (uint64_t)sp, arch_task_context_get_address_space(&t->ctx));
}

struct cpu_local* task_this_cpu(void) {
    return arch_task_get_cpu_local();
}

struct cpu_local* get_cpu_local(void) {
    return arch_task_get_cpu_local();
}

struct cpu_local* get_cpu_local_by_id(uint32_t cpu_id) {
    if (cpu_id >= g_cpu_count || cpu_id >= ORTHOX_MAX_CPUS) return NULL;
    return &g_cpu_locals[cpu_id];
}

void task_set_cpu_count(uint32_t cpu_count) {
    if (cpu_count == 0) cpu_count = 1;
    if (cpu_count > ORTHOX_MAX_CPUS) cpu_count = ORTHOX_MAX_CPUS;
    g_cpu_count = cpu_count;
}

uint32_t task_get_cpu_count(void) {
    return g_cpu_count;
}

int task_get_runq_stats(struct orth_runq_stat* out, uint32_t max_count) {
    uint32_t cpu_count = task_get_cpu_count();
    uint32_t started = smp_get_started_cpu_count();
    uint32_t count = 0;
    uint64_t flags;

    if (!out || max_count == 0) return -1;
    if (cpu_count == 0) cpu_count = 1;
    if (started == 0 || started > cpu_count) started = cpu_count;

    flags = spin_lock_irqsave(&g_task_lock);
    for (uint32_t cpu_id = 0; cpu_id < started && count < max_count; cpu_id++) {
        struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
        struct task* current;
        uint32_t migratable = 0;
        uint32_t affined_tasks = 0;
        uint32_t affined_ready = 0;
        uint32_t affined_running = 0;
        uint32_t affined_sleeping = 0;
        uint32_t blocked_ready = 0;
        uint32_t blocked_running = 0;
        uint32_t blocked_sleeping = 0;
        if (!cpu) continue;
        current = cpu->current_task;
        for (struct task* t = task_list; t; t = t->next) {
            enum task_migration_reason migrate_reason;
            if (task_is_idle_task(t)) continue;
            if (t->state == TASK_DEAD) continue;
            if ((uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity) != cpu_id) continue;
            affined_tasks++;
            migrate_reason = task_migration_reason_locked(t);
            if (t->state == TASK_READY) {
                affined_ready++;
                if (migrate_reason == TASK_MIGRATE_OK) migratable++;
                else blocked_ready++;
            } else if (t->state == TASK_RUNNING) {
                affined_running++;
                if (migrate_reason != TASK_MIGRATE_OK) blocked_running++;
            } else if (t->state == TASK_SLEEPING || t->state == TASK_IO_WAIT) {
                affined_sleeping++;
                if (migrate_reason != TASK_MIGRATE_OK) blocked_sleeping++;
            }
        }
        out[count].cpu_id = cpu_id;
        out[count].runq_count = cpu->runq_count;
        out[count].total_load = cpu->runq_count;
        out[count].affined_tasks = affined_tasks;
        out[count].affined_ready = affined_ready;
        out[count].affined_running = affined_running;
        out[count].affined_sleeping = affined_sleeping;
        out[count].blocked_ready = blocked_ready;
        out[count].blocked_running = blocked_running;
        out[count].blocked_sleeping = blocked_sleeping;
        out[count].current_pid = current ? current->pid : -1;
        out[count].current_state = current ? (int32_t)current->state : -1;
        out[count].runq_head_pid = cpu->runq_head ? cpu->runq_head->pid : -1;
        out[count].runq_tail_pid = cpu->runq_tail ? cpu->runq_tail->pid : -1;
        out[count].current_is_idle = (current && current == cpu->idle_task) ? 1U : 0U;
        out[count].migratable_count = migratable;
        if (current && current != cpu->idle_task && current->state == TASK_RUNNING) {
            out[count].total_load++;
        }
        count++;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return (int)count;
}

static inline struct task* get_current_task_raw(void) {
    struct cpu_local* cpu = arch_task_get_cpu_local();
    return cpu ? cpu->current_task : NULL;
}

static int default_task_cpu_affinity(void) {
    struct cpu_local* cpu = arch_task_get_cpu_local();
    return cpu ? (int)cpu->cpu_id : 0;
}

static int normalize_cpu_affinity(uint32_t cpu_id) {
    uint32_t cpu_count = task_get_cpu_count();
    if (cpu_count == 0) cpu_count = 1;
    if (cpu_id >= cpu_count) return 0;
    return (int)cpu_id;
}

static uint32_t task_cpu_load_locked(uint32_t cpu_id) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    uint32_t load = 0;
    if (!cpu) return UINT32_MAX;
    load = cpu->runq_count;
    if (cpu->current_task && cpu->current_task != cpu->idle_task &&
        cpu->current_task->state == TASK_RUNNING) {
        load++;
    }
    return load;
}

static uint32_t task_choose_rebalance_cpu_locked(uint32_t fallback_cpu) {
    uint32_t cpu_count = task_get_cpu_count();
    uint32_t started = smp_get_started_cpu_count();
    uint32_t best_cpu;
    uint32_t best_load;

    if (cpu_count == 0) cpu_count = 1;
    if (started == 0 || started > cpu_count) started = cpu_count;
    best_cpu = (uint32_t)normalize_cpu_affinity(fallback_cpu);
    best_load = task_cpu_load_locked(best_cpu);

    for (uint32_t cpu_id = 0; cpu_id < started; cpu_id++) {
        const struct smp_cpu_info* cpu = smp_get_cpu_info(cpu_id);
        if (cpu && cpu->started) {
            uint32_t load = task_cpu_load_locked(cpu_id);
            if (load < best_load) {
                best_load = load;
                best_cpu = cpu_id;
            }
        }
    }
    return best_cpu;
}

static uint32_t task_rebalance_ready_task_locked(struct task* t) {
    uint32_t current_cpu;
    uint32_t best_cpu;
    uint32_t current_load;
    uint32_t best_load;

    if (!task_can_migrate_locked(t)) {
        return t ? (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity) : 0;
    }

    current_cpu = (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
    best_cpu = task_choose_rebalance_cpu_locked(current_cpu);
    if (best_cpu == current_cpu) return current_cpu;

    current_load = task_cpu_load_locked(current_cpu);
    best_load = task_cpu_load_locked(best_cpu);
    if (current_load <= best_load + 1) return current_cpu;

    task_runq_push_locked(t, best_cpu);
    return (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
}

static uint32_t choose_spawn_cpu_locked(uint32_t fallback_cpu) {
    static uint32_t next_cpu = 0;
    uint32_t cpu_count = task_get_cpu_count();
    uint32_t started = smp_get_started_cpu_count();
    uint32_t best_cpu;
    uint32_t best_load;
    if (cpu_count == 0) cpu_count = 1;
    if (started == 0 || started > cpu_count) started = cpu_count;
    if (started <= 1) return (uint32_t)normalize_cpu_affinity(fallback_cpu);

    best_cpu = (uint32_t)normalize_cpu_affinity(fallback_cpu);
    best_load = task_cpu_load_locked(best_cpu);
    uint32_t start = next_cpu;
    for (uint32_t attempt = 0; attempt < started; attempt++) {
        uint32_t cpu_id = (start + attempt) % started;
        const struct smp_cpu_info* cpu = smp_get_cpu_info(cpu_id);
        if (cpu && cpu->started) {
            uint32_t load = task_cpu_load_locked(cpu_id);
            if (load < best_load) {
                best_load = load;
                best_cpu = cpu_id;
            }
        }
    }
    next_cpu = (best_cpu + 1) % started;
    return best_cpu;
}

uint32_t task_choose_fork_cpu_locked(uint32_t fallback_cpu) {
    return task_get_fork_spread()
        ? choose_spawn_cpu_locked(fallback_cpu)
        : (uint32_t)normalize_cpu_affinity(fallback_cpu);
}

static void init_cpu_local(struct cpu_local* cpu, uint32_t cpu_id,
                           struct task* current, struct task* idle,
                           uint64_t kernel_stack) {
    if (!cpu) return;
    cpu->cpu_id = cpu_id;
    cpu->self = cpu;
    cpu->current_task = current;
    cpu->idle_task = idle;
    cpu->runq_head = 0;
    cpu->runq_tail = 0;
    cpu->resched_pending = 0;
    cpu->runq_count = 0;
    cpu->kernel_lock_depth = 0;
    cpu->kernel_stack = kernel_stack;
    cpu->user_stack = 0;
}

static void task_refresh_cpu_local_msrs(struct cpu_local* cpu) {
    if (!cpu) return;
    arch_task_set_cpu_local(cpu);
}

static struct cpu_local* task_get_ready_cpu_locked(uint32_t cpu_id) {
    return get_cpu_local_by_id(normalize_cpu_affinity(cpu_id));
}

static int task_is_idle_task(struct task* t) {
    return t && t->pid == 0;
}

static enum task_migration_reason task_migration_reason_locked(struct task* t) {
    if (!t) return TASK_MIGRATE_BLOCK_NULL;
    if (task_is_idle_task(t)) return TASK_MIGRATE_BLOCK_IDLE;
    if (t->state != TASK_READY) return TASK_MIGRATE_BLOCK_NOT_READY;
    if (!t->on_runq) return TASK_MIGRATE_BLOCK_NOT_ON_RUNQ;
    return TASK_MIGRATE_OK;
}

static int task_can_migrate_locked(struct task* t) {
    return task_migration_reason_locked(t) == TASK_MIGRATE_OK;
}

static void task_runq_remove_locked(struct task* t) {
    struct cpu_local* cpu;
    if (!t || !t->on_runq) return;
    KASSERT(t->state == TASK_READY);
    cpu = task_get_ready_cpu_locked((uint32_t)t->cpu_affinity);
    KASSERT(cpu != 0);
    if (t->runq_prev) t->runq_prev->runq_next = t->runq_next;
    else cpu->runq_head = t->runq_next;
    if (t->runq_next) t->runq_next->runq_prev = t->runq_prev;
    else cpu->runq_tail = t->runq_prev;
    t->runq_prev = 0;
    t->runq_next = 0;
    t->on_runq = 0;
    if (cpu->runq_count > 0) cpu->runq_count--;
}

static void task_runq_push_locked(struct task* t, uint32_t cpu_id) {
    struct cpu_local* cpu;
    if (!t || task_is_idle_task(t)) return;
    KASSERT(t->state == TASK_READY);
    cpu_id = (uint32_t)normalize_cpu_affinity(cpu_id);
    if (t->on_runq) task_runq_remove_locked(t);
    cpu = task_get_ready_cpu_locked(cpu_id);
    if (!cpu) return;
    t->cpu_affinity = (int)cpu_id;
    t->runq_prev = cpu->runq_tail;
    t->runq_next = 0;
    if (cpu->runq_tail) cpu->runq_tail->runq_next = t;
    else cpu->runq_head = t;
    cpu->runq_tail = t;
    t->on_runq = 1;
    cpu->runq_count++;
    KASSERT(cpu->runq_head != 0);
    KASSERT(cpu->runq_tail != 0);
}

static struct task* task_runq_pop_locked(struct cpu_local* cpu) {
    struct task* t;
    if (!cpu) return 0;
    t = cpu->runq_head;
    if (!t) return 0;
    task_runq_remove_locked(t);
    return t;
}
struct task* task_runq_pop_locked_internal(struct cpu_local* cpu) {
    return task_runq_pop_locked(cpu);
}

int task_is_idle_task_internal(struct task* t) {
    return task_is_idle_task(t);
}

void task_refresh_cpu_local_msrs_internal(struct cpu_local* cpu) {
    task_refresh_cpu_local_msrs(cpu);
}

void task_write_user_fs_base_internal(uint64_t fs_base) {
    arch_task_apply_user_tls(fs_base);
}

static int task_set_affinity_locked(struct task* t, uint32_t cpu_id) {
    if (!t) return -1;
    cpu_id = (uint32_t)normalize_cpu_affinity(cpu_id);
    if (!task_can_migrate_locked(t)) {
        if (t->cpu_affinity == (int)cpu_id) return 0;
        return -1;
    }
    task_runq_push_locked(t, cpu_id);
    return 0;
}

static int task_mark_ready_on_cpu_locked(struct task* t, uint32_t cpu_id) {
    if (!t) return -1;
    if (task_is_idle_task(t)) return -1;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD) return -1;
    t->state = TASK_READY;
    t->sleep_until_ms = 0;
    task_runq_push_locked(t, cpu_id);
    return 0;
}

static int task_mark_sleeping_locked(struct task* t) {
    if (!t) return -1;
    task_runq_remove_locked(t);
    t->sleep_until_ms = 0;
    t->state = TASK_SLEEPING;
    return 0;
}

static int task_mark_io_wait_locked(struct task* t) {
    if (!t) return -1;
    task_runq_remove_locked(t);
    t->sleep_until_ms = 0;
    t->state = TASK_IO_WAIT;
    return 0;
}

static int task_mark_io_wait_until_locked(struct task* t, uint64_t deadline_ms) {
    if (!t) return -1;
    task_runq_remove_locked(t);
    t->sleep_until_ms = deadline_ms;
    t->state = TASK_IO_WAIT;
    return 0;
}

static int task_mark_zombie_locked(struct task* t, int exit_status) {
    if (!t) return -1;
    task_runq_remove_locked(t);
    t->exit_status = exit_status;
    t->state = TASK_ZOMBIE;
    return 0;
}

static int task_wake_locked(struct task* t) {
    if (!t) return -1;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD) return -1;
    t->sleep_until_ms = 0;
    return task_mark_ready_on_cpu_locked(t, (uint32_t)t->cpu_affinity);
}

int task_wake_locked_internal(struct task* t) {
    return task_wake_locked(t);
}

uint32_t task_normalize_cpu_affinity_internal(uint32_t cpu_id) {
    return (uint32_t)normalize_cpu_affinity(cpu_id);
}

static int task_reap_locked(struct task* t) {
    struct task** link;
    if (!t) return -1;
    if (t->state != TASK_ZOMBIE && t->state != TASK_DEAD) return -1;
    /* 降りきっていない。task_reap が待ってから来るので、ここに来るのは
     * 待たずに呼んだときだけ。解放せずに断る */
    if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) return -1;
    KASSERT(!task_is_idle_task(t));
    task_assert_stack_aligned(t->kstack_top);
    task_assert_address_space_owned(t);
    task_runq_remove_locked(t);

    link = &task_list;
    while (*link && *link != t) {
        link = &(*link)->next;
    }
    if (*link != t) return -1;
    *link = t->next;

    t->state = TASK_DEAD;

    if (t->deferred_cr3 && t->deferred_cr3 != arch_vm_kernel_address_space()) {
        arch_vm_destroy_user_address_space(t->deferred_cr3);
        t->deferred_cr3 = 0;
    }

    uint64_t _free_as = arch_task_context_get_address_space(&t->ctx);
    if (_free_as && _free_as != arch_vm_kernel_address_space()) {
        arch_vm_destroy_user_address_space(_free_as);
        arch_task_context_set_address_space(&t->ctx, 0);
    }

    if (t->kstack_top) {
        uint64_t kstack_phys = VIRT_TO_PHYS((void*)(t->kstack_top - 4 * PAGE_SIZE));
        pmm_free((void*)kstack_phys, 4);
        t->kstack_top = 0;
    }

    t->next = 0;
    t->runq_prev = 0;
    t->runq_next = 0;
    t->on_runq = 0;
    return free_task_struct(t);
}

void task_bind_cpu_local(uint32_t cpu_id, struct task* current, struct task* idle,
                         uint64_t kernel_stack) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    if (!cpu) return;
    init_cpu_local(cpu, cpu_id, current, idle, kernel_stack);
    /* 起動時にこの CPU で走っているタスク。schedule を通らずに載っている */
    if (current) current->on_cpu = 1;
}

/* **切り替えを終えた CPU で、降りたタスクの on_cpu を落とす。**
 * schedule() の arch_context_switch の後と、初めて走るタスクの入口
 * (idle / task_main / fork の子の復帰先) で呼ぶ。ここに来た時点で、
 * 降りたタスクのスタックと ctx はもう使っていない */
void task_finish_switch(void) {
    struct cpu_local* cpu = get_cpu_local();
    struct task* prev;
    if (!cpu) return;
    prev = cpu->switched_from;
    if (!prev) return;
    cpu->switched_from = 0;
    __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
}

void task_install_cpu_local(uint32_t cpu_id) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    task_refresh_cpu_local_msrs(cpu);
}

struct task* get_current_task(void) {
    return get_current_task_raw();
}

task_context_t* task_current_context(void) {
    struct task* t = get_current_task_raw();
    return t ? &t->ctx : 0;
}

static void* kernel_memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void kernel_strcpy(char* dst, const char* src, size_t size) {
    size_t i = 0;
    if (!dst || size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void init_console_fds(struct task* t) {
    if (!t) return;
    (void)fs_init_console_fd(&t->fds[0], O_RDONLY);
    (void)fs_init_console_fd(&t->fds[1], O_WRONLY);
    (void)fs_init_console_fd(&t->fds[2], O_WRONLY);
}

static struct task* alloc_task_struct(void) {
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    void* phys = pmm_alloc(task_pages);
    if (!phys) return NULL;
    struct task* t = (struct task*)PHYS_TO_VIRT(phys);
    kernel_memset(t, 0, sizeof(struct task));
    /* **0 埋めのままにしない。** umask=0 は「作ったファイルが誰でも書ける」。
     * fork の子は親から引き継ぐので (task_fork.c)、ここが効くのは根だけ */
    t->umask = 022;
    return t;
}

static int free_task_struct(struct task* t) {
    if (!t) return -1;
    int task_pages = (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free((void*)VIRT_TO_PHYS(t), task_pages);
    return 0;
}

struct task* task_alloc_struct(void) {
    return alloc_task_struct();
}

int task_free_struct(struct task* t) {
    return free_task_struct(t);
}

static struct arch_task_user_state task_user_state(const struct task* t) {
    struct arch_task_user_state state;
    state.entry_pc = t ? t->user_entry : 0;
    state.user_sp = t ? t->user_stack : 0;
    state.arg0 = t ? t->user_argc : 0;
    state.arg1 = t ? t->user_argv : 0;
    state.arg2 = t ? t->user_envp : 0;
    return state;
}

void task_main(void) {
    struct task* t;
    task_finish_switch();
    t = get_current_task();
    struct arch_task_user_state state = task_user_state(t);
    arch_task_sync_user_state(&t->ctx, &state);
    arch_task_enter_initial_user(&state, &t->ctx, &t->os_stack_ptr);
    (void)task_mark_zombie(t, 0);
    while(1) schedule();
}

void task_init(void) {
    // タスク構造体のサイズに合わせて必要なページを確保
    struct task* t = alloc_task_struct();
    struct task* idle = task_create_idle(0);
    if (!t) return;
    if (!idle) return;
    spinlock_init(&g_task_lock);
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    t->pid = next_pid++;
    t->pgid = t->pid;
    t->sid = t->pid;
    t->state = TASK_RUNNING;
    t->cpu_affinity = default_task_cpu_affinity();
    arch_task_context_set_address_space(&t->ctx, arch_vm_kernel_address_space());
    uint64_t sp = arch_task_read_current_stack_pointer();
    t->kstack_top = (sp & ~(PAGE_SIZE - 1)) + PAGE_SIZE; 
    t->os_stack_ptr = t->kstack_top;
    t->mmap_end = USER_MMAP_BASE_VADDR;
    t->user_fs_base = 0;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    struct cpu_local* cpu = &g_cpu_locals[0];
    init_cpu_local(cpu, 0, t, idle, t->kstack_top);
    task_list = t;
    spin_unlock_irqrestore(&g_task_lock, flags);
    init_console_fds(t);
    task_install_cpu_local(0);
    puts("Task system initialized.\r\n");
}

int task_set_affinity(struct task* t, uint32_t cpu_id) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_set_affinity_locked(t, cpu_id);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_mark_ready_on_cpu(struct task* t, uint32_t cpu_id) {
    int ret;
    uint32_t target_cpu;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    target_cpu = (uint32_t)normalize_cpu_affinity(cpu_id);
    ret = task_mark_ready_on_cpu_locked(t, target_cpu);
    /* 起床/READY 化時の rebalance は禁止。対象タスクが task_mark_* 直後で
     * まだ schedule() に達しておらず ctx 未保存のまま元 CPU で実行中の
     * 場合、別 CPU が古い ctx で同一タスクを二重実行しカーネルスタックを
     * 破壊する (make -j4 で BKL depth 破壊として実測)。同一 CPU への
     * 積み戻しは schedule() の next==current ガードで安全。負荷分散は
     * fork 時 spread (未実行タスクなので安全) に任せる。 */
    spin_unlock_irqrestore(&g_task_lock, flags);
    if (ret >= 0) {
        task_request_resched_cpu(target_cpu);
    }
    return ret;
}

int task_mark_ready_on_cpu_locked_internal(struct task* t, uint32_t cpu_id) {
    return task_mark_ready_on_cpu_locked(t, cpu_id);
}

uint32_t task_rebalance_ready_task_locked_internal(struct task* t) {
    return task_rebalance_ready_task_locked(t);
}

int task_mark_sleeping(struct task* t) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_mark_sleeping_locked(t);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_mark_io_wait(struct task* t) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_mark_io_wait_locked(t);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_mark_io_wait_until(struct task* t, uint64_t deadline_ms) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_mark_io_wait_until_locked(t, deadline_ms);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

/*
 * 「寝ると決めたが、まだ切り替わる前に条件が揃った」ときに自分の就寝を取り消す。
 *
 * task_wake() を自分に対して使ってはいけない。あちらは runqueue へ積むので、
 * まだ走行中のタスクが runqueue にも載った状態になり、別 CPU が同じタスクを
 * 走らせ得る。ここは状態を戻すだけで積まない。
 *
 * 既に誰かが起こしていた (TASK_READY になっている) 場合は何もしない。
 * その場合は runqueue に正しく載っているので、呼び出し側の kernel_yield() で
 * 通常どおり選び直される。
 */
int task_cancel_sleep(struct task* t) {
    int ret = -1;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    if (t && (t->state == TASK_SLEEPING || t->state == TASK_IO_WAIT)) {
        t->sleep_until_ms = 0;
        t->state = TASK_RUNNING;
        ret = 0;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_mark_zombie(struct task* t, int exit_status) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    ret = task_mark_zombie_locked(t, exit_status);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

int task_wake(struct task* t) {
    int ret;
    uint32_t target_cpu = 0;
    if (!t) return -1;
    {
        uint64_t flags = spin_lock_irqsave(&g_task_lock);
        ret = task_wake_locked(t);
        if (ret >= 0) {
            /* rebalance 禁止の理由は task_mark_ready_on_cpu のコメント参照。 */
            target_cpu = (uint32_t)normalize_cpu_affinity((uint32_t)t->cpu_affinity);
        }
        spin_unlock_irqrestore(&g_task_lock, flags);
    }
    if (ret < 0) return ret;
    task_request_resched_cpu(target_cpu);
    return 0;
}

/* **親が死ぬときに、その子を始末する。**2 本立てになっている:
 *
 *   既に zombie の子    その場で reap する
 *   まだ生きている子    ppid を 1 に付け替える
 *
 * **効いているのは前者だけ。**Orthox には孤児を回収する init が居らず、
 * pid 1 は sh で自分が起動したコマンドしか wait しないので、**付け替えても
 * 引き取り手が居ない。**前者が「親のいない zombie」の蓄積を止めている。
 *
 * 2026-08-31 に linux_syscall.c 側 (aarch64 / riscv64) にだけ入り、
 * **x86 には入っていなかった。**2026-09-08 に同じ手順 ( cmd & を 3 回) で
 * 両方を走らせて測ったところ、親のいない zombie の数は
 *
 *     aarch64  0 のまま
 *     x86      0 -> 1 -> 2 -> 3 と単調に増える
 *
 * だったので、実装をここへ出して両方から呼ぶことにした。
 *
 * **task_list はロックの中で辿る (2026-09-19)。**以前はロック無しで辿って
 * おり、aarch64 / riscv64 の syscall は BKL を取らないので、別の hart の
 * 回収でノードが外れて解放されると巡回が壊れた (wait4 と同じ穴。
 * task_find_zombie_child の注記)。
 *
 * 生きている子はロックの中で pid 1 へ付け替える。zombie の子は 1 人ずつ
 * ロックの中で見つけ、ロックの外で回収する (task_reap は降りきるまで待つ
 * ので、ロックを持ったまま呼べない)。zombie になるのも task のロックの中
 * なので、付け替えと取りこぼしは起きない */
void task_reap_orphans_of(int pid) {
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    for (struct task* t = task_list; t; t = t->next) {
        if (t->ppid == pid && t->state != TASK_ZOMBIE) t->ppid = 1;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);

    for (;;) {
        struct task* z = task_find_zombie_child(pid, -1, 0);
        if (!z || task_reap(z) < 0) break;
    }
}

/* ---- 子の終了を待つ親 -----------------------------------------------------
 *
 * **2026-08-30 に aarch64 / riscv64 だけを直した形をここへ出した
 * (2026-09-09)。**それまで x86 の sys_wait4 は kernel_yield() で回し続けて
 * おり、**待っている親はコアを 1 本 100% 焼く。**aarch64 側では実機の
 * [pc] 計器が 2 本とも wait4 を指し、「configure はほぼ逐次のはずなのに
 * 2 コアが 100%」の正体がこれだった (日報2026-08-29 の G-2)。
 *
 * **時間切れつきで寝る。**task_mark_zombie は 5 か所から呼ばれるのに、
 * 親を起こす経路は exit にしか無い (Ctrl-C / SIGSEGV / kill は通らない)。
 * 素直に寝かせると、それらの経路で親が永久に起きず**空回りより悪くなる。**
 * 上限を置けば、起こし損ねても「その回だけ遅い」で済む。
 *
 * **SIGCHLD の番号はここに持ち込まない。**x86 は 20、aarch64 / riscv64 は
 * 17 を使っており、揃っていない。シグナルの送出は呼び手の側に残してある
 * (揃えるなら別の変更として、壊れたときに切り分けられる形でやる)。
 *
 * wait_queue は spinlock_init と head=0 しかしないので、静的変数は
 * ゼロ初期化のまま使える (kernel/wait.c の wait_queue_init 参照)。 */
static struct wait_queue g_child_exit_wq;

struct task_child_wait { int ppid; int want; };

/* 待ち行列のロックの中から呼ばれる。task のロックを取る順序
 * (待ち行列 -> task) は wait_event が task_mark_io_wait を呼ぶのと同じ */
static int task_child_zombie_ready(void* arg) {
    struct task_child_wait* w = (struct task_child_wait*)arg;
    return task_find_zombie_child(w->ppid, w->want, 0) != 0;
}

/* 子が終わった。**親を特定せずに全部起こす** —— 起こされた側は述語で
 * 「自分の子か」を見るので、無関係な親は寝直すだけ */
void task_child_exit_wake(void) {
    wake_up_all(&g_child_exit_wq);
}

/* 子が zombie になるまで寝る。timeout_ms で自力でも起きる。
 * 戻り値は見ない —— 呼び手は起きてから task_list を歩き直す */
void task_wait_child_exit(int ppid, int want, uint64_t timeout_ms) {
    struct task_child_wait w;
    w.ppid = ppid;
    w.want = want;
    (void)wait_event_timeout(&g_child_exit_wq, task_child_zombie_ready, &w, timeout_ms);
}

/* pid からタスクを探す。**ロックを取らない。**
 *
 * x86 は同じ役目の find_task_by_pid_locked (kernel/sys_proc.c) を持つが、
 * あちらは kernel_lock_held() を要求して警告を出す。x86 の syscall 入口は
 * BKL を握るので問題ないが、aarch64 / riscv64 の syscall 入口 (SVC / ECALL)
 * は BKL を取らずに割り込みだけ開けて処理する (kernel/aarch64/usermode.c,
 * kernel/riscv64/trap.c) ので、そちらの経路で使うにはロック無しの版が要る。
 * exit の親探しはどちらの経路からも呼ばれるので、ロック無しのこちらを使う
 * (2026-09-13、別実装 29 組の exit/wait4 を畳んだときに用意した)。
 *
 * **巡回は task のロックの中でする (2026-09-19)。**ロック無しだと、別の hart
 * の回収でノードが外れて巡回が壊れる。返したポインタが生きている保証は
 * 呼び手の側の事情による (exit の親探しなら、子が居る間は親は回収されない) */
struct task* task_find_by_pid(int pid) {
    struct task* found = 0;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    for (struct task* t = task_list; t; t = t->next) {
        if (t->pid == pid) {
            found = t;
            break;
        }
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return found;
}

/* Ctrl-C / Ctrl-\ の配送。プロセスグループ pgid の全員 (exclude_pid を除く)
 * に sig の保留を立てて zombie にする。落とした pid を pids に最大 max 個
 * 入れ、全体の数を返す。**巡回も zombie 化もロックの中** (以前は各 arch の
 * 呼び手がロック無しで task_list を辿っていた。task_find_zombie_child の注記) */
int task_kill_pgrp(int pgid, int exclude_pid, int sig, int exit_status,
                   int* pids, int max) {
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    for (struct task* t = task_list; t; t = t->next) {
        if (t->pgid != pgid || t->pid == exclude_pid) continue;
        if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD) continue;
        t->sig_pending |= (1ULL << sig);
        (void)task_mark_zombie_locked(t, exit_status);
        if (pids && n < max) pids[n] = t->pid;
        n++;
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    return n;
}

/* 診断用。task_list の先頭から max 個をロックの中で写す。**出力はロックの
 * 外で** (UART に出している間ずっと割り込みを止めないため)。*more には
 * 写しきれなかったタスクが居たかを入れる */
int task_snapshot(struct task_snapshot* out, int max, int* more) {
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    struct task* t = task_list;
    for (; t && n < max; t = t->next, n++) {
        out[n].pid = t->pid;
        out[n].ppid = t->ppid;
        out[n].state = t->state;
        for (int i = 0; i < (int)sizeof(out[n].comm) - 1; i++) {
            out[n].comm[i] = t->comm[i];
            if (!t->comm[i]) break;
        }
        out[n].comm[sizeof(out[n].comm) - 1] = 0;
    }
    if (more) *more = (t != 0);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return n;
}

/* wait4 用。親 parent_pid の子 (pid が -1 なら誰でも) を探し、zombie が
 * 居ればそれを返す。*found_child には子が 1 人でも居たかを入れる。
 *
 * **task_list はロックの中で辿る (2026-09-19)。**以前の wait4 はロック無しで
 * 辿っており、別の hart の親が自分の子を回収すると、task_reap_locked が
 * そのノードを外して next を 0 にしてから解放するので、ちょうどその上に
 * いた巡回はそこで打ち切られ (解放済みも読み)、自分の子を見落として
 * ECHILD を返した (cowstress 4 hart で "FAIL waitpid")。
 * 返したタスクを回収できるのは親だけなので、ロックを放した後も消えない */
struct task* task_find_zombie_child(int parent_pid, int pid, int* found_child) {
    struct task* zombie = 0;
    int found = 0;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    for (struct task* t = task_list; t; t = t->next) {
        if (t->ppid != parent_pid || (pid != -1 && t->pid != pid)) continue;
        found = 1;
        if (t->state == TASK_ZOMBIE) {
            zombie = t;
            break;
        }
    }
    spin_unlock_irqrestore(&g_task_lock, flags);
    if (found_child) *found_child = found;
    return zombie;
}

/* **CPU から降りきるまで待ってから回収する (2026-09-19)。**
 *
 * zombie は task_mark_zombie の後も、schedule() がロックを放してから
 * arch_context_switch を終えるまで、自分のカーネルスタックと task 構造体
 * (ctx にレジスタを保存する) を使っている。以前はここで待たずに解放して
 * おり、別の hart の親が wait4 で拾うと、**走行中のスタック・task 構造体・
 * ページテーブルが解放されて使い回されうる。**riscv64 の cowstress (4 hart)
 * で、割り込みの窓 (kernel/riscv64/entry.S / trap.S) を塞いだ後もこれだけで
 * 30 回中 2 回止まり、うち 1 回は 2 つの hart がカーネルの中で不正な番地を
 * 読んで落ちていた。ここも直した後 (wait4 の巡回をロックの中へ移したものと
 * 合わせて) は 60 回続けて止まらなかった。
 *
 * zombie は必ずすぐ降りる (sys_exit は kernel_yield を回し、走らせるものが
 * 無ければ idle へ切り替わる) ので、待つのは切り替え 1 回ぶん。
 * **ロックの外で待つ。**降りる側の schedule() が同じロックを取る */
/* ★一時的な計器 (2026-09-20)。on_cpu が落ちない相手と、そのとき各 CPU が
 * 何を載せているかを吐く。原因が分かったら消す */
int task_reap(struct task* t) {
    int ret;
    uint64_t flags;
    if (t) {
        while (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("" ::: "memory");
        }
    }
    flags = spin_lock_irqsave(&g_task_lock);
    ret = task_reap_locked(t);
    spin_unlock_irqrestore(&g_task_lock, flags);
    return ret;
}

struct task* task_create_on_cpu(uint64_t entry, uint64_t user_rsp, uint32_t cpu_id) {
    struct task* t = alloc_task_struct();
    if (!t) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
    t->pid = next_pid++;
    t->pgid = t->pid;
    t->sid = t->pid;
    task_mark_ready_on_cpu_locked(t, cpu_id);
    t->user_entry = entry;
    t->user_stack = user_rsp;
    t->user_stack_top = USER_STACK_TOP_VADDR;
    t->user_stack_bottom = 0;
    t->user_stack_guard = 0;
    t->mmap_end = USER_MMAP_BASE_VADDR;
    t->user_fs_base = 0;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    init_console_fds(t);
    void* kstack_phys = pmm_alloc(4);
    t->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    t->os_stack_ptr = t->kstack_top;
    arch_address_space_t user_as = arch_vm_create_user_address_space();
    if (!user_as) {
        spin_unlock_irqrestore(&g_task_lock, flags);
        return NULL;
    }
    arch_task_context_set_address_space(&t->ctx, user_as);
    uint64_t* sp = (uint64_t*)(t->kstack_top - 8);
    *sp = (uint64_t)task_main;
    *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    arch_task_context_init_kernel_entry(&t->ctx, (uint64_t)task_main, (uint64_t)sp, user_as);
    t->next = task_list;
    task_list = t;
    spin_unlock_irqrestore(&g_task_lock, flags);
    return t;
}

struct task* task_create(uint64_t entry, uint64_t user_rsp) {
    return task_create_on_cpu(entry, user_rsp, (uint32_t)default_task_cpu_affinity());
}

struct task* task_create_idle(uint32_t cpu_id) {
    struct task* t = alloc_task_struct();
    if (!t) return NULL;
    t->pid = 0;
    t->state = TASK_RUNNING;
    t->cpu_affinity = (int)cpu_id;
    arch_task_context_set_address_space(&t->ctx, arch_vm_kernel_address_space());
    t->mmap_end = USER_MMAP_BASE_VADDR;
    t->timeslice_ticks = TASK_TIMESLICE_TICKS;
    kernel_strcpy(t->cwd, "/", sizeof(t->cwd));
    void* kstack_phys = pmm_alloc(4);
    if (!kstack_phys) return NULL;
    t->kstack_top = (uint64_t)PHYS_TO_VIRT(kstack_phys) + 4 * PAGE_SIZE;
    t->os_stack_ptr = t->kstack_top;
    task_init_idle_context(t);
    arch_task_context_init_fp_state(&t->ctx);
    (void)cpu_id;
    return t;
}
