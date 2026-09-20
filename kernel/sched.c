#include "task_internal.h"
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

    /* 入口で呼び忘れた経路があっても、次の schedule() で必ず落ちる
     * (ここは降りたタスクとは別のスタックの上) */
    task_finish_switch();

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
    /* **prev の on_cpu は切り替えが終わるまで落とさない。**ロックを放した
     * 後も、prev のスタックの上で arch_context_switch が prev->ctx へ
     * 書く。落とすのは次のタスクの側 (task_finish_switch) */
    next->on_cpu = 1;
    cpu->switched_from = prev;
    /* **ロックは放すが、割り込みはここで開けない (2026-09-20)。**
     * switched_from を書いてから task_finish_switch が prev の on_cpu を
     * 落とすまでの間に割り込まれると、割り込みの入口 (x86 は
     * kernel/x86_64/idt.c の interrupt_dispatch) が BKL を取りに行く。
     * その BKL を wait4 -> task_reap が握って on_cpu が落ちるのを待って
     * いると、落とす役目の CPU が BKL 待ちで止まったまま噛み合う
     * (2026-09-19 の日報 §8 の x86 デッドロック)。
     * **切り替えの区間は入口から出口まで閉じたままにする。**ここで閉じ、
     * 切り替え先へは各 arch の切り替えが閉じたまま戻し (x86 は
     * kernel/x86_64/task_switch.S)、開け直すのは戻った側。切り替え先が
     * この続きに戻らない場合 (初めて走るタスク) は、それぞれの入口が
     * 自分で開ける —— idle は arch_task_idle_wait_once の sti、
     * fork の子と task_main はユーザーへ降りる iretq */
    task_unlock_keep_irq();
    arch_context_switch(&next->ctx, &prev->ctx);
    task_finish_switch();
    irq_restore(flags);
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
