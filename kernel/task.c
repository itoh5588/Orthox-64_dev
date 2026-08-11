#include <stdint.h>
#include <stddef.h>
#include "kassert.h"
#include "task.h"
#include "task_internal.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "limine.h"
#include "net_socket.h"
#include "net.h"
#include "smp.h"
#include "spinlock.h"
#include "lapic.h"

static struct cpu_local g_cpu_locals[ORTHOX_MAX_CPUS];
struct task* task_list = NULL;
static int next_pid = 1;
static uint32_t g_cpu_count = 1;
static spinlock_t g_task_lock;

extern volatile struct limine_module_request module_request;

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
    struct task* t = get_current_task();
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

int task_reap(struct task* t) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_task_lock);
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
