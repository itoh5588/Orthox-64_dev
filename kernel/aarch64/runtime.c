/*
 * 共有層の下敷き (M3c-2a)。
 *
 * kernel/riscv64/runtime.c と同じ位置づけ。共有層 (kernel 直下の C) が
 * 「どのアーキでもあるもの」として呼ぶ関数を、aarch64 の実物に繋ぐ。
 *
 * **kernel/spinlock.c は使わない。** riscv64 と同じで、ロックと割り込みの
 * 開け閉めはアーキ固有なのでこちらに置く。共有版は x86 の割り込み操作を
 * 直接書いている。
 *
 * まだ載せていないもの (M3c-2b で埋める):
 *   kernel_yield    schedule() を呼ぶ形にする。いまは M3c-1 の器を回す
 *   kernel_lock_*   CPU ごとの深さにする。いまは単一 CPU 前提の 1 本
 *   コンソール入力   riscv64_console_* 相当。PL011 の受信割り込みが要る
 */
#include <stdint.h>
#include <stddef.h>
#include "aarch64/boot.h"
#include "aarch64/task.h"
#include "aarch64/vm.h"
#include "spinlock.h"

/* 物理 → カーネル VA の差。**pmm_init が実際の値を入れる。**
 * ここでは定義だけ (riscv64 も runtime.c に置いている) */
uint64_t g_hhdm_offset;

uint64_t aarch64_timer_freq(void);
uint64_t aarch64_timer_ticks(void);

/* ---- 時刻 ----------------------------------------------------------------
 *
 * **tick 数ではなくカウンタから出す。** tick は 10ms 刻みなので、
 * それを ms に使うと分解能が 10ms のまま固定される。CNTPCT_EL0 は
 * 単調増加のカウンタなので、周波数で割れば素直に ms になる。 */
static inline uint64_t read_cntpct(void) {
    uint64_t v;
    __asm__ volatile("isb");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t arch_time_now_ms(void) {
    uint64_t freq = aarch64_timer_freq();
    /* 0 のまま割ると落ちる。timer.c と同じ既定値に退く */
    if (freq == 0) freq = 62500000ULL;
    return read_cntpct() / (freq / 1000ULL);
}

uint64_t arch_time_cpu_ms(uint32_t cpu_id) {
    (void)cpu_id;
    return arch_time_now_ms();
}

/* ---- 割り込みの開け閉め --------------------------------------------------
 *
 * riscv64 の sstatus.SIE に当たるのが DAIF の I (bit 7)。
 * **戻すときは元の値をそのまま書かず、I だけを復元する。** 他のビット
 * (F/A/D) を巻き戻すと、呼び出し元が意図して変えたものを消す */
#define DAIF_I  (1ULL << 7)

uint64_t irq_save_disable(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2");
    return daif;
}

void irq_restore(uint64_t flags) {
    if (flags & DAIF_I) {
        __asm__ volatile("msr daifset, #2");
    } else {
        __asm__ volatile("msr daifclr, #2");
    }
}

/* ---- スピンロック --------------------------------------------------------
 *
 * **CPU 1 本でも要る。** 割り込みハンドラと通常の実行が同じデータを
 * 触るので、irqsave の版が本体。単純な exchange のループ */
void spinlock_init(spinlock_t* lock) {
    if (!lock) return;
    lock->locked = 0;
}

void spin_lock(spinlock_t* lock) {
    if (!lock) return;
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        /* SMP に進んだら wfe / sevl で待つ形にする。1 本のうちは意味が無い */
        __asm__ volatile("yield");
    }
}

void spin_unlock(spinlock_t* lock) {
    if (!lock) return;
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(spinlock_t* lock) {
    uint64_t flags = irq_save_disable();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    irq_restore(flags);
}

/* ---- BKL -----------------------------------------------------------------
 *
 * **いまは CPU 1 本前提の深さ 1 本。** riscv64 は cpu_local に持たせて
 * いるが、そちらは共有タスク層が居て初めて成り立つ。M3c-2b で
 * cpu_local に移すこと。 */
static spinlock_t g_kernel_lock;
static uint32_t g_kernel_lock_depth;

void kernel_lock_enter(void) {
    uint64_t flags = irq_save_disable();
    if (g_kernel_lock_depth == 0) spin_lock(&g_kernel_lock);
    g_kernel_lock_depth++;
    irq_restore(flags);
}

void kernel_lock_exit(void) {
    uint64_t flags = irq_save_disable();
    if (g_kernel_lock_depth != 0) {
        g_kernel_lock_depth--;
        if (g_kernel_lock_depth == 0) spin_unlock(&g_kernel_lock);
    }
    irq_restore(flags);
}

int kernel_lock_held(void) {
    return g_kernel_lock_depth != 0;
}

/* ---- コンソール ----------------------------------------------------------
 *
 * 共有層は puts / puthex で出す。**aarch64 の UART に繋ぐだけ。**
 * 1 行が並行に割れないよう、出力の単位を囲む (日報2026-08-09 追6-5) */
void puts(const char* s) {
    aarch64_uart_puts(s);
}

void puthex(uint64_t value) {
    aarch64_uart_puthex64(value);
}

int64_t sys_write_serial(const char* buf, size_t count) {
    if (!buf) return -1;
    aarch64_console_begin();
    for (size_t i = 0; i < count; i++) aarch64_uart_putchar(buf[i]);
    aarch64_console_end();
    return (int64_t)count;
}

void kernel_panic(const char* file, int line, const char* func, const char* expr) {
    aarch64_console_begin();
    puts("\n*** KERNEL PANIC ***\n");
    puts("expr: "); puts(expr ? expr : "(null)");
    puts("\nfunc: "); puts(func ? func : "(null)");
    puts("\nfile: "); puts(file ? file : "(null)");
    puts(":0x"); puthex((uint64_t)(uint32_t)line);
    puts("\nHALTING...\n");
    aarch64_console_end();
    for (;;) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

/* ==========================================================================
 * 共有層の自己診断 (M3c-2a)
 *
 * **コンパイルが通ったことは、動くことの証拠にならない。** ここで確かめる
 * のは「共有層が実際に使う経路」だけ:
 *
 *   1. pmm.h の形で確保できて、PHYS_TO_VIRT で触れること
 *      → g_hhdm_offset が効いている証拠。0 のままなら物理を返して落ちる
 *   2. 参照カウントが効くこと
 *      → 1 回目の pmm_free で返してしまうと、まだ使われているページを配る
 *   3. arch_vm_* でアドレス空間を作って張って外せること
 *      → 張った直後に get_phys が同じ物理を返し、外した後は 0 を返す
 *   4. **後始末でページ数が元に戻ること**
 *      → 戻らなければどこかで漏れている
 * ========================================================================== */
#include "pmm.h"
#include "vmm.h"
#include "arch_vm.h"

/* どこにも使われていない VA。**新しく作った空間なので何でもよい**が、
 * ユーザーのコードが居る 0x400000 とは離しておく */
#define SELFTEST_VA  0x0000000010000000ULL

static void report(const char* label, int ok, int* all) {
    aarch64_console_begin();
    puts(label);
    puts(ok ? "  ok\n" : "  BAD\n");
    aarch64_console_end();
    if (!ok) *all = 0;
}

int aarch64_shared_layer_selftest(void) {
    int ok = 1;
    uint64_t pages_before = pmm_get_allocated_pages();
    uint64_t pages_after;
    void* page;
    volatile uint64_t* p;
    arch_address_space_t as;
    uint64_t got;

    aarch64_console_begin();
    puts("--- M3c-2a: 共有層 (pmm.h / arch_vm_*) ---\n");
    aarch64_console_end();

    /* 1. 確保して PHYS_TO_VIRT 経由で触る */
    page = pmm_alloc(1);
    report("  pmm_alloc :", page != 0, &ok);
    if (!page) goto done;

    p = (volatile uint64_t*)PHYS_TO_VIRT(page);
    *p = 0x5aa5c33c5aa5c33cULL;
    report("  hhdm rw   :", *p == 0x5aa5c33c5aa5c33cULL, &ok);

    /* 2. 参照カウント。**1 回目の free では返らないこと**まで見る */
    pmm_incref(page);
    report("  incref    :", pmm_get_ref(page) == 2, &ok);
    pmm_free(page, 1);
    report("  free x1   :", pmm_get_ref(page) == 1, &ok);

    /* 3. アドレス空間を作って張って外す */
    as = arch_vm_create_user_address_space();
    report("  create as :", as != 0, &ok);
    if (as) {
        arch_vm_map_page(as, SELFTEST_VA, (uint64_t)(uintptr_t)page,
                         arch_vm_user_page_flags(1, 0));
        got = arch_vm_get_phys(as, SELFTEST_VA);
        report("  map/get   :", got == (uint64_t)(uintptr_t)page, &ok);

        arch_vm_unmap_page(as, SELFTEST_VA);
        got = arch_vm_get_phys(as, SELFTEST_VA);
        report("  unmap     :", got == 0, &ok);

        arch_vm_destroy_user_address_space(as);
    }

    /* 4. 後始末。**確保したぶんが戻ること。**
     * テーブルは arch_vm_map_page が作るので、そのぶんは残る
     * (テーブルの解放は M3c-2b。日報の未実施表にある) */
    pmm_free(page, 1);
    report("  free x2   :", pmm_get_ref(page) == 0, &ok);

done:
    pages_after = pmm_get_allocated_pages();
    aarch64_console_begin();
    puts("  pages     : ");
    puthex(pages_before);
    puts(" -> ");
    puthex(pages_after);
    /* 張るのに使った L2 / L3 のテーブル 2 枚が残る。**「0 に戻る」と
     * 書かずに、残る枚数を明示しておく。** ここが変わったら気づける */
    puts(pages_after == pages_before + 2 ? "  ok (テーブル 2 枚が未解放)\n"
                                         : "  BAD (見込みと違う)\n");
    if (pages_after != pages_before + 2) ok = 0;
    puts(ok ? "aarch64-shared-ok\n" : "aarch64-shared-BAD\n");
    aarch64_console_end();
    return ok;
}

/* ==========================================================================
 * SMP (M3c-2b) — **CPU 1 本しか無い**
 *
 * riscv64 は SBI HSM で副 hart を起こす (kernel/riscv64/smp.c 187 行)。
 * aarch64 は PSCI (HVC/SMC 経由) を使うことになるが、まだ入れていない。
 * ここでは「1 本しか無い」を正しく答えるだけにする。
 *
 * **0 を返して誤魔化さない。** cpu 0 の情報は本物を返し、範囲外は 0。
 * ========================================================================== */
#include "smp.h"
#include "task.h"

static struct smp_cpu_info g_cpus[1];
static int g_cpus_ready;

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index != 0) return 0;
    if (!g_cpus_ready) {
        g_cpus[0].cpu_index = 0;
        g_cpus[0].processor_id = 0;
        g_cpus[0].lapic_id = 0;
        g_cpus[0].is_bsp = 1;
        g_cpus[0].started = 1;
        g_cpus_ready = 1;
    }
    return &g_cpus[0];
}

uint32_t smp_get_started_cpu_count(void) { return 1; }

/* **自分にしか送りようが無い。** 自 CPU への resched は
 * task_request_resched がフラグを立てるので、ここで何かする必要は無い。
 * 他 CPU が現れたら PSCI と GIC の SGI が要る */
void smp_send_resched_ipi(uint32_t cpu_id) {
    (void)cpu_id;
}

/* ---- コンソールの fd (M3c-2b) --------------------------------------------
 *
 * **まだ VFS を繋いでいない。** task_init が fds[0..2] を作ろうとするが、
 * aarch64 には fs.c を取り込んでいないので実体が無い。
 *
 * 0 (成功) を返すと「使える fd がある」ことになり、read/write が
 * 何も繋がっていない fd を触る。**失敗を返して、無いことを伝える。**
 * 呼ぶ側 (init_console_fds) は戻り値を捨てるが、fd は 0 のまま残る */
int fs_init_console_fd(file_descriptor_t* fd, int flags) {
    (void)fd;
    (void)flags;
    return -1;
}

/* ---- 譲る (M3c-2b) -------------------------------------------------------
 *
 * riscv64 と同じく、**BKL を全部手放してから schedule()、戻ったら取り直す。**
 * 持ったまま切り替えると、次のタスクが同じロックを待って進めなくなる */
void kernel_yield(void) {
    uint32_t depth = g_kernel_lock_depth;
    for (uint32_t i = 0; i < depth; i++) kernel_lock_exit();
    schedule();
    for (uint32_t i = 0; i < depth; i++) kernel_lock_enter();
}

/* ==========================================================================
 * 共有タスク層の自己診断 (M3c-2b)
 *
 * **リンクが通ったことは、動く証拠にならない。** 確かめるのは
 * 「共有スケジューラで実際に切り替わって戻ってくる」ことだけ。
 *
 * 寝て起きる 1 往復に、確かめたいものが全部入っている:
 *
 *   schedule()            走行中のタスクを降ろして idle を選ぶ
 *   arch_context_switch   aarch64_context_switch に繋がっている
 *   cpu_local             current_task / idle_task が引ける
 *   task_on_timer_tick    タイマ割り込みが共有層に届いている
 *   task_poll_sleep_wakeups  期限が来たタスクを起こす
 *   task_consume_resched  IRQ の出口で schedule() が呼ばれる
 *
 * **どれか 1 つでも欠けたら戻ってこない。** 逆に言えば、戻ってきた時点で
 * 全部繋がっている。経過時間を見るのは「即座に戻ってきた」= 切り替えて
 * いない場合を弾くため。
 * ========================================================================== */
#define SHARED_SLEEP_MS 50

int aarch64_shared_task_selftest(void) {
    struct task* cur;
    uint64_t t0, t1, elapsed;
    int ok = 1;

    aarch64_console_begin();
    puts("--- M3c-2b: 共有スケジューラ (kernel/sched.c) ---\n");
    aarch64_console_end();

    /* **乗り換えは task_init の前。** 後にすると、その隙のタイマ割り込みが
     * M3c-1 の器を叩き、まだ器のタスクが居る前提で切り替えようとする */
    aarch64_task_use_shared_scheduler();
    task_init();

    cur = get_current_task();
    report("  current   :", cur != 0, &ok);
    if (!cur) goto done;

    report("  cpu local :", get_cpu_local() != 0, &ok);
    report("  idle task :", get_cpu_local() && get_cpu_local()->idle_task != 0, &ok);

    /* **ここが本体。** 期限付きで寝て、タイマに起こしてもらう */
    t0 = arch_time_now_ms();
    task_mark_io_wait_until(cur, t0 + SHARED_SLEEP_MS);
    kernel_yield();
    t1 = arch_time_now_ms();
    elapsed = t1 - t0;

    aarch64_console_begin();
    puts("  sleep     : ");
    puthex(elapsed);
    /* 即座に戻ってきたら切り替えていない。**しきい値は境界に乗せない** —
     * 要求 50ms に対して 40ms 以上を合格とする (日報2026-08-09 追2-3) */
    puts(elapsed >= (SHARED_SLEEP_MS - 10) ? " ms  ok (寝て、タイマに起こされた)\n"
                                           : " ms  BAD (切り替わっていない)\n");
    aarch64_console_end();
    if (elapsed < (SHARED_SLEEP_MS - 10)) ok = 0;

done:
    aarch64_console_begin();
    puts(ok ? "aarch64-sched-ok\n" : "aarch64-sched-BAD\n");
    aarch64_console_end();
    return ok;
}

/* ---- まだ無いもの --------------------------------------------------------
 *
 * **黙って成功を返さない。** 「何もしない」で正しいものだけをここに置く。
 * ネットワークは aarch64 に無いので、ポーリングの必要も無い */
int bottom_half_run(void) {
    return 0;
}

int net_needs_poll_fallback(void) {
    return 0;
}

void net_poll(void) {
}
