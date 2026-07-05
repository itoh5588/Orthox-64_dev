#include "task_internal.h"
#include "gdt.h"
#include "lapic.h"
#include "net.h"
#include "smp.h"
#include "spinlock.h"
#include "bottom_half.h"

extern struct task* task_list;
extern void switch_context(struct task_context* next_ctx, struct task_context* prev_ctx);

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
    tss_set_stack_for_cpu(cpu->cpu_id, next->kstack_top);
    cpu->kernel_stack = next->kstack_top;
    task_refresh_cpu_local_msrs_internal(cpu);
    task_write_user_fs_base_internal(next->user_fs_base);
    task_unlock_irqrestore(flags);
    switch_context(&next->ctx, &prev->ctx);
}

void task_on_timer_tick(void) {
    struct task* current_task = get_current_task();
    struct cpu_local* cpu = task_this_cpu();
    uint64_t now = 0;
    if (cpu && cpu->cpu_id == 0) {
        now = lapic_get_ticks_ms();
        uint64_t flags = task_lock_irqsave();
        struct task* t = task_list;
        while (t) {
            if ((t->state == TASK_SLEEPING || t->state == TASK_IO_WAIT) &&
                t->sleep_until_ms != 0 && t->sleep_until_ms <= now) {
                t->sleep_until_ms = 0;
                task_wake_locked_internal(t);
            }
            t = t->next;
        }
        task_unlock_irqrestore(flags);
    }
    if (!current_task || current_task->state != TASK_RUNNING) return;
#if ORTHOX_MEM_PROGRESS
    if (now == 0) now = lapic_get_ticks_ms();
    task_trace_progress_tick_internal(current_task, now);
#endif
    if (current_task->timeslice_ticks > 1) {
        current_task->timeslice_ticks--;
        return;
    }
    current_task->timeslice_ticks = TASK_TIMESLICE_TICKS;
    task_request_resched();
}

void task_idle_loop(int poll_network) {
    __asm__ volatile("sti");
    for (;;) {
        bottom_half_run();
        if (poll_network && net_needs_poll_fallback()) {
            net_poll();
        }
        __asm__ volatile("hlt");
        bottom_half_run();
        if (task_consume_resched()) {
            kernel_yield();
        }
    }
}
