#include "task_internal.h"
#include "gdt.h"
#include "lapic.h"
#include "net.h"
#include "smp.h"
#include "spinlock.h"
#include "bottom_half.h"
#include "usb.h"

extern struct task* task_list;
extern void arch_context_switch(struct arch_task_context* next_ctx, struct arch_task_context* prev_ctx);

static int g_fork_spread_enabled = 1;

int task_set_fork_spread(int enabled) {
    g_fork_spread_enabled = enabled ? 1 : 0;
    return 0;
}

int task_get_fork_spread(void) {
    return g_fork_spread_enabled;
}

void task_request_resched(void) {
    struct cpu_local* cpu = task_this_cpu();
    if (cpu) cpu->resched_pending = 1;
}

void task_request_resched_cpu(uint32_t cpu_id) {
    struct cpu_local* cpu = get_cpu_local_by_id(cpu_id);
    struct cpu_local* self = task_this_cpu();
    if (!cpu) return;
    cpu->resched_pending = 1;
    if (!self || self->cpu_id != cpu_id) {
        smp_send_resched_ipi(cpu_id);
    }
}

int task_consume_resched(void) {
    struct cpu_local* cpu = task_this_cpu();
    if (!cpu || !cpu->resched_pending) return 0;
    cpu->resched_pending = 0;
    return 1;
}

void schedule(void) {
    struct cpu_local* cpu = task_this_cpu();
    struct task* current_task = cpu ? cpu->current_task : NULL;
    struct task* next;
    struct task* prev;
    uint64_t flags;

    if (!current_task) return;

    flags = task_lock_irqsave();
    next = task_runq_pop_locked_internal(cpu);
    if (!next) {
        if (current_task->state == TASK_RUNNING) {
            task_unlock_irqrestore(flags);
            return;
        }
        next = cpu->idle_task;
    }
    if (next == current_task) {
        // A wakeup can land between marking ourselves blocked and reaching
        // here; the pop above already dequeued us, so restore RUNNING or the
        // next schedule() would drop this task from every queue.
        next->state = TASK_RUNNING;
        task_unlock_irqrestore(flags);
        return;
    }
    prev = current_task;
    if (prev->state == TASK_RUNNING && !task_is_idle_task_internal(prev)) {
        task_mark_ready_on_cpu_locked_internal(prev, (uint32_t)prev->cpu_affinity);
    }
    cpu->current_task = next;
    next->on_runq = 0;
    next->state = TASK_RUNNING;
    if (next->timeslice_ticks <= 0) next->timeslice_ticks = TASK_TIMESLICE_TICKS;
    cpu->kernel_stack = next->kstack_top;
    arch_task_prepare_schedule_switch(cpu->cpu_id, next->kstack_top, cpu, next->user_fs_base);
    task_unlock_irqrestore(flags);
    arch_context_switch(&next->ctx, &prev->ctx);
}

/*
 * sleep_until_ms を過ぎたタスクを起こす。戻り値は起こした数。
 *
 * task_lock の外から呼べる。どの CPU から呼んでも安全なので、タイマー割り込み
 * (task_on_timer_tick) だけでなく、カーネル内でブロック待ちを回す側からも
 * 直接呼ぶ (riscv64 の kernel_yield など。理由は下の task_on_timer_tick の
 * コメント参照)。
 */
int task_poll_sleep_wakeups(void) {
    /* 起床させた CPU の集合。task_wake_locked_internal() は runqueue に
     * 積むだけで resched_pending を立てないため、ここで要求しないと
     * 起床したタスクは走行中タスクのタイムスライスが尽きるまで
     * (TASK_TIMESLICE_TICKS 分) 待たされる。idle が回っている場合も
     * 同じで、idle は resched_pending を見て初めて kernel_yield() する。
     * task_wake() (ロック外の版) は同じことを既にやっている。 */
    uint64_t woken_cpus = 0;
    uint64_t now = arch_time_now_ms();
    int woken = 0;
    uint64_t flags = task_lock_irqsave();
    struct task* t = task_list;
    while (t) {
        if ((t->state == TASK_SLEEPING || t->state == TASK_IO_WAIT) &&
            t->sleep_until_ms != 0 && t->sleep_until_ms <= now) {
            t->sleep_until_ms = 0;
            if (task_wake_locked_internal(t) >= 0) {
                uint32_t target =
                    task_normalize_cpu_affinity_internal((uint32_t)t->cpu_affinity);
                if (target < 64U) woken_cpus |= (1ULL << target);
                woken++;
            }
        }
        t = t->next;
    }
    task_unlock_irqrestore(flags);
    /* IPI を伴うのでロックの外で。自 CPU 分はフラグを立てるだけ */
    for (uint32_t id = 0; woken_cpus != 0 && id < 64U; id++) {
        if (woken_cpus & (1ULL << id)) {
            woken_cpus &= ~(1ULL << id);
            task_request_resched_cpu(id);
        }
    }
    return woken;
}

void task_on_timer_tick(void) {
    struct task* current_task = get_current_task();
    struct cpu_local* cpu = task_this_cpu();
    if (cpu && cpu->cpu_id == 0) {
        (void)task_poll_sleep_wakeups();
    }
    if (!current_task || current_task->state != TASK_RUNNING) return;
#if ORTHOX_MEM_PROGRESS
    task_trace_progress_tick_internal(current_task, arch_time_now_ms());
#endif
    if (current_task->timeslice_ticks > 1) {
        current_task->timeslice_ticks--;
        return;
    }
    current_task->timeslice_ticks = TASK_TIMESLICE_TICKS;
    task_request_resched();
}

/* **usb.c を繋いでいないアーキテクチャでは何もしない。**
 * riscv64 のカーネルは USB を持たず、ここで undefined symbol になる。
 * usb.c が居るときはそちらの強い定義が勝つ */
__attribute__((weak)) void usb_hotplug_poll(void) {}

void task_idle_loop(int poll_network) {
    for (;;) {
        bottom_half_run();
        if (poll_network && net_needs_poll_fallback()) {
            net_poll();
        }
        /* **USB の抜き差しを見る。**タイマ割り込みからは呼べない —
         * 制御転送は 1 回 ms 単位かかる。ここは通常のタスク文脈で、
         * 中で 500ms に 1 回に絞っている (kernel/usb.c) */
        usb_hotplug_poll();
        arch_task_idle_wait_once();
        bottom_half_run();
        if (task_consume_resched()) {
            kernel_yield();
        }
    }
}
