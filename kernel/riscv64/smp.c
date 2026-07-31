/*
 * riscv64 SMP: SBI HSM で副 hart を起動し、共有スケジューラの idle ループへ載せる。
 *
 * 前提となる規約:
 *   - カーネル実行中の tp = hart index (start.S / trap.S が保証する)
 *   - hart 固有状態 (trap scratch / cpu_local) は hart index で引く配列
 *   - カーネル本体は大きなカーネルロック (g_riscv64_kernel_lock) で直列化される
 *
 * QEMU virt の hart id は 0..n-1 なので cpu index と 1:1 で対応させている。
 */
#include <stddef.h>
#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/sbi.h"
#include "riscv64/trap.h"
#include "riscv64/vm.h"
#include "smp.h"
#include "spinlock.h"
#include "task.h"

extern void riscv64_secondary_start(void);

/* riscv64_uart_puts は呼び出し単位でしか直列化されないため、複数 hart から
 * 出すログは 1 行を 1 バッファに組み立ててから 1 回で出す */
static void smp_log_hex(const char* prefix, uint64_t value) {
    static const char hex[] = "0123456789abcdef";
    char line[96];
    size_t i = 0;
    if (prefix) {
        while (prefix[i] && i + 24 < sizeof(line)) {
            line[i] = prefix[i];
            i++;
        }
    }
    line[i++] = '0';
    line[i++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        line[i++] = hex[(value >> shift) & 0xFULL];
    }
    line[i++] = '\n';
    line[i] = '\0';
    riscv64_uart_puts(line);
}

static struct smp_cpu_info g_riscv64_cpus[RISCV64_MAX_HARTS];
/* cpu index -> hart id。boot hart は必ず cpu 0 (OpenSBI が選ぶ boot hart は
 * hart 0 とは限らないため、そのまま hart id を CPU 番号にはできない) */
static uint64_t g_riscv64_cpu_hartid[RISCV64_MAX_HARTS];
static uint32_t g_riscv64_hart_count = 1;
static volatile uint32_t g_riscv64_started_harts = 1;

uint32_t riscv64_smp_hart_count(void) {
    return g_riscv64_hart_count;
}

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index >= g_riscv64_hart_count) return 0;
    return &g_riscv64_cpus[cpu_index];
}

uint32_t smp_get_started_cpu_count(void) {
    return __atomic_load_n(&g_riscv64_started_harts, __ATOMIC_ACQUIRE);
}

void smp_send_resched_ipi(uint32_t cpu_id) {
    if (cpu_id >= g_riscv64_hart_count) return;
    (void)riscv64_sbi_send_ipi(1ULL << g_riscv64_cpu_hartid[cpu_id], 0);
}

/* 副 hart のエントリ (start.S の riscv64_secondary_start から呼ばれる)。
 * sp はブートスタックの自 hart スライス、tp は cpu index が設定済み。 */
void riscv64_secondary_main(uint64_t cpu_index_arg) {
    uint32_t cpu_index = (uint32_t)cpu_index_arg;
    struct task* idle;

    /* 起動直後は satp=0 (bare)。カーネルは恒等マップなのでそのまま動くが、
     * ユーザー空間切替に備えてカーネルのアドレス空間を有効化しておく */
    riscv64_vm_activate_address_space(riscv64_vm_kernel_address_space());
    riscv64_trap_init();

    /* 共有スケジューラ構造を触るのでカーネルロックを取る。
     * この時点では自 hart の cpu_local が未設定 = kernel_lock_enter/exit は
     * depth 管理ではなく生の spin lock として振る舞う。したがってロック保持中に
     * task_install_cpu_local を呼んではいけない (exit 側が depth==0 を見て
     * 解放せずに戻り、全 hart がデッドロックする) */
    kernel_lock_enter();
    idle = task_create_idle(cpu_index);
    if (idle) {
        task_bind_cpu_local(cpu_index, idle, idle, idle->kstack_top);
    }
    kernel_lock_exit();

    if (idle) {
        task_install_cpu_local(cpu_index);
        g_riscv64_cpus[cpu_index].started = 1;
    }

    if (!idle) {
        riscv64_uart_puts("[smp] hart idle task alloc failed\n");
        riscv64_wait_forever();
    }

    riscv64_trap_set_kernel_stack(idle->kstack_top);
    riscv64_timer_init();
    riscv64_interrupts_enable();

    __atomic_add_fetch(&g_riscv64_started_harts, 1, __ATOMIC_SEQ_CST);

    smp_log_hex("[smp] cpu online: ", cpu_index);

    task_idle_loop(0);
    riscv64_wait_forever();
}

/* 存在する hart を SBI HSM で数え、boot hart 以外を起動する。
 * task_init() の後 (idle タスクを作れる状態) に呼ぶこと。 */
void riscv64_smp_start_secondaries(void) {
    const riscv64_boot_info_t* boot = riscv64_boot_info();
    uint64_t self = boot ? boot->hart_id : 0;
    uint32_t found = 1;

    /* cpu 0 = boot hart。残りの hart を 1 番から詰めて割り当てる */
    g_riscv64_cpu_hartid[0] = self;
    g_riscv64_cpus[0].cpu_index = 0;
    g_riscv64_cpus[0].processor_id = (uint32_t)self;
    g_riscv64_cpus[0].lapic_id = (uint32_t)self;
    g_riscv64_cpus[0].is_bsp = 1;
    g_riscv64_cpus[0].started = 1;

    for (uint64_t h = 0; h < RISCV64_MAX_HARTS && found < RISCV64_MAX_HARTS; h++) {
        riscv64_sbi_ret_t st;
        if (h == self) continue;
        st = riscv64_sbi_hart_status(h);
        if (st.error != 0) continue;   /* 存在しない hart */
        g_riscv64_cpu_hartid[found] = h;
        g_riscv64_cpus[found].cpu_index = found;
        g_riscv64_cpus[found].processor_id = (uint32_t)h;
        g_riscv64_cpus[found].lapic_id = (uint32_t)h;
        g_riscv64_cpus[found].is_bsp = 0;
        g_riscv64_cpus[found].started = 0;
        found++;
    }
    g_riscv64_hart_count = found;
    task_set_cpu_count(found);

    smp_log_hex("[smp] cpus detected: ", found);

    for (uint32_t cpu = 1; cpu < found; cpu++) {
        riscv64_sbi_ret_t ret = riscv64_sbi_hart_start(g_riscv64_cpu_hartid[cpu],
                                                       (uint64_t)(uintptr_t)riscv64_secondary_start,
                                                       cpu);
        if (ret.error != 0) {
            smp_log_hex("[smp] hart start failed: ", g_riscv64_cpu_hartid[cpu]);
        }
    }

    /* 全 hart が idle に入るまで待つ (最大 ~2 秒。QEMU virt の timebase は 10MHz) */
    {
        uint64_t deadline = riscv64_read_time() + 20000000ULL;
        while (smp_get_started_cpu_count() < found &&
               riscv64_read_time() < deadline) {
            __asm__ volatile("nop" ::: "memory");
        }
    }
    smp_log_hex("[smp] cpus online: ", smp_get_started_cpu_count());
}
