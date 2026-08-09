/*
 * 最小のタスク管理とラウンドロビン (M3c-1)。
 *
 * **共有スケジューラ (kernel/sched.c) にはまだ繋いでいない。** あちらは
 * task_internal.h / cpu_local / spinlock / bottom_half / smp を芋づるで
 * 要求するので、まず「切り替えの器」だけを自前で通す。接続は M3c-2。
 *
 * ここで確かめたいのは 3 つ:
 *   1. 明示的に譲ったときに切り替わる
 *   2. **タイマ割り込みでも切り替わる** (プリエンプション)
 *   3. ユーザータスクの切り替えで TTBR0 も入れ替わり、
 *      それぞれのアドレス空間が保たれる
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/task.h"
#include "aarch64/vm.h"

uint64_t aarch64_pmm_alloc(uint64_t pages);
void aarch64_vm_switch_user_space(uint64_t root_pa);

/* カーネルスタックは 16KB。**タスクごとに別。**
 * EL0 実行中に割り込みが入ると CPU が SP_EL1 にフレームを積むので、
 * 共有すると別のタスクのフレームを踏む */
#define AARCH64_KSTACK_PAGES 4
#define AARCH64_KSTACK_SIZE  (AARCH64_KSTACK_PAGES * 0x1000ULL)

static aarch64_task_t g_tasks[AARCH64_MAX_TASKS];
static int g_current;
static int g_task_count;
static uint64_t g_switches;
static volatile int g_resched;
static int g_sched_on;

/* ---- プリエンプトを止める区間 -------------------------------------------
 *
 * **1 行を出している途中で切り替わると、行が割れる。**
 *
 *   el0 ticks : 0x0000000000000  [EL0] resumed after permission fault
 *   bad ptr   : 0xfffffffffffffff2  ok (-EFAUL  [EL0] resumed ...
 *
 * カーネルの動作自体は正しく aarch64-user-ok も出るが、判定が行単位の
 * grep なので「数が合わない」形で落ちる。実測で 12 回中 5 回。
 * **M3c-1 でユーザータスクを 2 本並行にしたときから入っていた。**
 *
 * ここで止めるのは**切り替えだけ**で、割り込みは開けたまま。tick は
 * 数え続けるので el0 ticks の判定を壊さない。印 (g_resched) も消さないので、
 * 区間を出た後の最初の IRQ の出口で切り替わる。
 *
 * CPU 1 本を前提にしている。**SMP に進んだら CPU ごとに持ったうえで、
 * 出力そのものにロックが要る** (別の CPU は止まらない)。 */
static int g_preempt_off;

/* タイマ割り込みは g_preempt_off を読むだけなので、単一 CPU では
 * この読み書きに保護は要らない */
void aarch64_preempt_disable(void) { g_preempt_off++; }
void aarch64_preempt_enable(void)  { if (g_preempt_off > 0) g_preempt_off--; }
int  aarch64_preempt_disabled(void) { return g_preempt_off != 0; }

aarch64_task_t* aarch64_task_current(void) { return &g_tasks[g_current]; }
uint64_t aarch64_task_switch_count(void)   { return g_switches; }
uint64_t aarch64_task_counter(int id) {
    return (id >= 0 && id < AARCH64_MAX_TASKS) ? g_tasks[id].counter : 0;
}
int aarch64_task_state(int id) {
    return (id >= 0 && id < AARCH64_MAX_TASKS) ? g_tasks[id].state : AARCH64_TASK_FREE;
}

/* いま走っているものを 0 番として登録する。**自分自身はスタックも
 * コンテキストも既に持っている**ので、枠を確保するだけでよい */
void aarch64_task_init(void) {
    for (int i = 0; i < AARCH64_MAX_TASKS; i++) g_tasks[i].state = AARCH64_TASK_FREE;
    g_tasks[0].id = 0;
    g_tasks[0].state = AARCH64_TASK_RUNNING;
    g_tasks[0].ttbr0 = aarch64_vm_user_root_pa_current();
    g_current = 0;
    g_task_count = 1;
    g_switches = 0;
    g_resched = 0;
    g_sched_on = 1;
}

/* タスクが入口から戻ってきたときの受け皿。**入口が return しても
 * 迷子にならないように、必ずここを経由させる** */
static void aarch64_task_trampoline(void);

int aarch64_task_create(void (*entry)(void), uint64_t ttbr0) {
    int id = -1;
    uint64_t stack_pa, stack_top;

    for (int i = 1; i < AARCH64_MAX_TASKS; i++) {
        if (g_tasks[i].state == AARCH64_TASK_FREE) { id = i; break; }
    }
    if (id < 0) return -1;

    stack_pa = aarch64_pmm_alloc(AARCH64_KSTACK_PAGES);
    if (!stack_pa) return -1;
    stack_top = aarch64_phys_to_virt(stack_pa) + AARCH64_KSTACK_SIZE;

    for (unsigned i = 0; i < sizeof(g_tasks[id].ctx) / 8; i++) {
        ((uint64_t*)&g_tasks[id].ctx)[i] = 0;
    }
    /* **x19 に入口を仕込み、x30 はトランポリンにする。**
     * 最初の switch は「トランポリンへ戻る」形になり、そこから入口を呼ぶ。
     * 入口を直接 x30 に入れてもよいが、return されたときに迷子になる */
    g_tasks[id].ctx.x19 = (uint64_t)(uintptr_t)entry;
    g_tasks[id].ctx.x30 = (uint64_t)(uintptr_t)aarch64_task_trampoline;
    g_tasks[id].ctx.sp  = stack_top;
    g_tasks[id].ttbr0 = ttbr0;
    g_tasks[id].kstack_pa = stack_pa;
    g_tasks[id].counter = 0;
    g_tasks[id].id = id;
    g_tasks[id].state = AARCH64_TASK_READY;
    g_task_count++;
    return id;
}

/* x19 に入っている入口を呼び、戻ってきたら終わらせる。
 * **naked にして x19 をそのまま使う。** C の関数として書くと、
 * プロローグが x19 を潰す可能性がある */
__attribute__((naked)) static void aarch64_task_trampoline(void) {
    __asm__ volatile(
        "blr    x19\n"
        "bl     aarch64_task_exit\n"
        "b      .\n"
    );
}

/* 次に走らせるものを選ぶ。単純なラウンドロビン */
static int aarch64_task_pick_next(void) {
    for (int i = 1; i <= AARCH64_MAX_TASKS; i++) {
        int id = (g_current + i) % AARCH64_MAX_TASKS;
        if (g_tasks[id].state == AARCH64_TASK_READY) return id;
    }
    return -1;
}

static void aarch64_task_switch_to(int next_id) {
    int prev_id = g_current;
    aarch64_task_t* prev = &g_tasks[prev_id];
    aarch64_task_t* next = &g_tasks[next_id];

    if (prev->state == AARCH64_TASK_RUNNING) prev->state = AARCH64_TASK_READY;
    next->state = AARCH64_TASK_RUNNING;
    g_current = next_id;
    g_switches++;

    /* **アドレス空間も一緒に切り替える。** ここが AArch64 の素直な所で、
     * カーネルは TTBR1 に居るので TTBR0 だけ差し替えればよい。
     * riscv64 / x86 のようにカーネル領域の写しを作る必要が無い */
    if (next->ttbr0 && next->ttbr0 != prev->ttbr0) {
        aarch64_vm_switch_user_space(next->ttbr0);
    }

    aarch64_context_switch(&next->ctx, &prev->ctx);
    /* ここに戻ってきたときは、また自分の番になっている */
}

void aarch64_task_yield(void) {
    int next_id;
    uint64_t daif;

    /* **区間の中では明示的に譲っても切り替わらない。** 譲ったつもりで
     * 行が割れるより、区間を出るまで待たせるほうがよい */
    if (!g_sched_on || g_preempt_off) return;

    /* 選んでから切り替えるまでのあいだに割り込みが入ると、
     * 同じタスクを 2 か所から動かすことになる */
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2");

    next_id = aarch64_task_pick_next();
    if (next_id >= 0) aarch64_task_switch_to(next_id);

    __asm__ volatile("msr daif, %0" :: "r"(daif));
}

void aarch64_task_exit(void) {
    g_tasks[g_current].state = AARCH64_TASK_DONE;
    for (;;) aarch64_task_yield();
}

/* ---- 共有タスク層の受け口 (M3c-2a) --------------------------------------
 *
 * まだ共有層は載せていないが、arch_task_* の inline がこの 2 つを呼ぶので
 * 実体を置いておく。M3c-2b で共有層を繋ぐときにそのまま使える。 */
static struct cpu_local* g_cpu_local;
static uint64_t g_kernel_sp;

struct cpu_local* aarch64_task_get_cpu_local_impl(void) { return g_cpu_local; }
void aarch64_task_set_cpu_local_impl(struct cpu_local* cpu) { g_cpu_local = cpu; }

/* **AArch64 では例外で自動的に SP_EL1 に切り替わる**ので、riscv64 が
 * sscratch を手で入れ替えていたところは覚えておくだけでよい。
 * 実際に使うのは、EL0 へ降りる前に SP_EL1 をこの値にする所 (M3c-2b) */
void aarch64_trap_set_kernel_stack(uint64_t kernel_sp) { g_kernel_sp = kernel_sp; }
uint64_t aarch64_trap_kernel_stack(void) { return g_kernel_sp; }

/* タイマ割り込みから呼ぶ。**ここでは切り替えない。**
 * 割り込みハンドラの途中で切り替えると、まだ積んでいないものが出る。
 * 印だけ立てて、出口 (aarch64_task_resched_if_needed) で切り替える */
void aarch64_task_on_tick(void) {
    if (g_sched_on) g_resched = 1;
}

/* IRQ の出口で呼ぶ。**割り込みハンドラのフレームを積み終えた後**なので、
 * ここで切り替えれば、戻ってきたときに続きから再開できる */
void aarch64_task_resched_if_needed(void) {
    int next_id;
    if (!g_sched_on || !g_resched) return;
    /* **印は消さずに戻る。** 消すと、区間の中で入った tick のぶんの
     * 切り替えが 1 回まるごと消える */
    if (g_preempt_off) return;
    g_resched = 0;
    next_id = aarch64_task_pick_next();
    if (next_id >= 0) aarch64_task_switch_to(next_id);
}
