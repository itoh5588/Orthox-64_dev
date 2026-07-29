#include <stdint.h>
#include "riscv64/csr.h"
#include "riscv64/boot.h"
#include "riscv64/sbi.h"
#include "riscv64/syscall.h"
#include "riscv64/trap.h"
#include "net.h"
#include "spinlock.h"
#include "task.h"

extern void riscv64_trap_entry(void);

// trap.S が offset 0..32 を直接参照する。サイズ 64 バイト固定 (trap.S の
// `slli t2, tp, 6` が hart index からアドレスを算出するため変更不可)
struct riscv64_trap_scratch {
    uint64_t kernel_sp;   /*  0 */
    uint64_t saved_t1;    /*  8 */
    uint64_t saved_t2;    /* 16 */
    uint64_t saved_sp;    /* 24 */
    uint64_t hart_index;  /* 32 : カーネル実行中の tp に載せる値 */
    uint64_t reserved[3];
};

struct riscv64_trap_scratch g_riscv64_trap_scratch[RISCV64_MAX_HARTS];

static struct riscv64_trap_scratch* riscv64_trap_scratch_self(void) {
    uint64_t hart = riscv64_current_hart_index();
    if (hart >= RISCV64_MAX_HARTS) hart = 0;
    return &g_riscv64_trap_scratch[hart];
}

static uint64_t g_riscv64_timer_interval = 1000000ULL;
static uint64_t g_riscv64_timer_ticks;
static long g_riscv64_last_timer_error;
static uint64_t g_riscv64_last_timer_deadline;
static int g_riscv64_logged_timer_after_user_handoff;

static void riscv64_trap_rearm_current_kernel_stack(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu || !cpu->kernel_stack) return;
    // カーネル実行中は sscratch=0 を保つため kernel_sp フィールドのみ更新する。
    // sscratch への書き戻しは trap 出口 (ユーザー復帰時) が行う
    riscv64_trap_scratch_self()->kernel_sp = cpu->kernel_stack;
}

static void riscv64_timer_arm_next(void) {
    uint64_t now = riscv64_read_time();
    riscv64_sbi_ret_t ret;
    g_riscv64_last_timer_deadline = now + g_riscv64_timer_interval;
    ret = riscv64_sbi_set_timer(g_riscv64_last_timer_deadline);
    g_riscv64_last_timer_error = ret.error;
}

static void riscv64_handle_timer_interrupt(void) {
    kernel_lock_enter();
    if (riscv64_current_hart_index() == 0) {
        net_poll();
    }
    task_on_timer_tick();
    kernel_lock_exit();
}

static void riscv64_handle_ecall(riscv64_trap_frame_t* frame) {
    uint64_t sstatus;
    if (!frame) return;
    sstatus = riscv64_read_sstatus();
    riscv64_write_sstatus(sstatus | RISCV64_SSTATUS_SUM);
    riscv64_syscall_dispatch(frame);
    riscv64_write_sstatus(sstatus);
}

static void riscv64_trap_print_frame(const riscv64_trap_frame_t* frame) {
    if (!frame) return;
    riscv64_uart_puts("riscv64 trap\n");
    riscv64_uart_puts("  scause : 0x");
    riscv64_uart_puthex64(frame->scause);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sepc   : 0x");
    riscv64_uart_puthex64(frame->sepc);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  stval  : 0x");
    riscv64_uart_puthex64(frame->stval);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sstatus: 0x");
    riscv64_uart_puthex64(frame->sstatus);
    riscv64_uart_puts("\n");
}

void riscv64_trap_init(void) {
    struct riscv64_trap_scratch* scratch = riscv64_trap_scratch_self();
    scratch->hart_index = riscv64_current_hart_index();
    riscv64_write_sscratch(0);
    riscv64_write_stvec((uint64_t)(uintptr_t)riscv64_trap_entry);
    // 副 hart のブートログは冗長なので boot hart のみ出す
    if (scratch->hart_index == 0) {
        riscv64_uart_puts("  trap vector installed\n");
    }
}

void riscv64_trap_set_kernel_stack(uint64_t kernel_sp) {
    struct riscv64_trap_scratch* scratch = riscv64_trap_scratch_self();
    scratch->hart_index = riscv64_current_hart_index();
    scratch->kernel_sp = kernel_sp;
    riscv64_write_sscratch((uint64_t)(uintptr_t)scratch);
}

void riscv64_timer_init(void) {
    riscv64_write_sie(riscv64_read_sie() | RISCV64_SIE_STIE);
    riscv64_timer_arm_next();
    if (riscv64_current_hart_index() != 0) return;
    riscv64_uart_puts("  sbi timer armed\n");
    if (g_riscv64_last_timer_error != 0) {
        riscv64_uart_puts("  timer err: 0x");
        riscv64_uart_puthex64((uint64_t)g_riscv64_last_timer_error);
        riscv64_uart_puts("\n");
    }
}

void riscv64_trap_dispatch(riscv64_trap_frame_t* frame) {
    if (!frame) {
        riscv64_uart_puts("riscv64 trap: null frame\n");
        riscv64_wait_forever();
    }

    if (frame->scause == RISCV64_SCAUSE_ECALL_U || frame->scause == RISCV64_SCAUSE_ECALL_S) {
        riscv64_handle_ecall(frame);
        riscv64_trap_rearm_current_kernel_stack();
        return;
    }

    if (frame->scause == RISCV64_SCAUSE_SSOFT) {
        // resched IPI: sip.SSIP を落として、ユーザーからの割り込みなら切り替える
        riscv64_clear_sip_ssip();
        if ((frame->sstatus & RISCV64_SSTATUS_SPP) == 0 && !kernel_lock_held()) {
            if (task_consume_resched()) {
                kernel_yield();
            }
        }
        riscv64_trap_rearm_current_kernel_stack();
        return;
    }

    if (frame->scause == RISCV64_SCAUSE_BREAKPOINT) {
        frame->sepc += 2;
        riscv64_uart_puts("riscv64 breakpoint trap\n");
        riscv64_trap_rearm_current_kernel_stack();
        return;
    }

    if (frame->scause == RISCV64_SCAUSE_STIMER) {
        g_riscv64_timer_ticks++;
        riscv64_timer_arm_next();
        riscv64_handle_timer_interrupt();
        if (g_riscv64_timer_ticks == 1) {
            riscv64_uart_puts("riscv64 supervisor timer interrupt\n");
        }
        if (riscv64_user_handoff_started() && !g_riscv64_logged_timer_after_user_handoff) {
            g_riscv64_logged_timer_after_user_handoff = 1;
            riscv64_uart_puts("riscv64 timer interrupt after user handoff\n");
        }
        // ユーザーモードからの割り込みに限りプリエンプトする。
        // trap フレームはタスク固有のカーネルスタック上にあるので、ここで
        // arch_context_switch しても復帰時に同じフレームから sret できる。
        // カーネル実行中の割り込みでは切り替えない (カーネル側は非プリエンプティブ)。
        if ((frame->sstatus & RISCV64_SSTATUS_SPP) == 0 && !kernel_lock_held()) {
            if (task_consume_resched()) {
                kernel_yield();
            }
        }
        riscv64_trap_rearm_current_kernel_stack();
        return;
    }

    riscv64_trap_print_frame(frame);
    riscv64_wait_forever();
}
