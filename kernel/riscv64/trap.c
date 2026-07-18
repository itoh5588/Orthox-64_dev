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

struct riscv64_trap_scratch {
    uint64_t kernel_sp;
    uint64_t saved_t1;
    uint64_t saved_t2;
    uint64_t saved_sp;
};

struct riscv64_trap_scratch g_riscv64_trap_scratch;

static uint64_t g_riscv64_timer_interval = 1000000ULL;
static uint64_t g_riscv64_timer_ticks;
static long g_riscv64_last_timer_error;
static uint64_t g_riscv64_last_timer_deadline;
static int g_riscv64_logged_timer_after_user_handoff;

static void riscv64_trap_rearm_current_kernel_stack(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu) return;
    riscv64_trap_set_kernel_stack(cpu->kernel_stack);
}

static void riscv64_timer_arm_next(void) {
    uint64_t now = riscv64_read_time();
    riscv64_sbi_ret_t ret;
    g_riscv64_last_timer_deadline = now + g_riscv64_timer_interval;
    ret = riscv64_sbi_set_timer(g_riscv64_last_timer_deadline);
    g_riscv64_last_timer_error = ret.error;
}

static void riscv64_handle_timer_interrupt(void) {
    const riscv64_boot_info_t* boot = riscv64_boot_info();
    kernel_lock_enter();
    if (boot && boot->hart_id == 0) {
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
    riscv64_write_sscratch(0);
    riscv64_write_stvec((uint64_t)(uintptr_t)riscv64_trap_entry);
    riscv64_uart_puts("  trap vector installed\n");
}

void riscv64_trap_set_kernel_stack(uint64_t kernel_sp) {
    g_riscv64_trap_scratch.kernel_sp = kernel_sp;
    riscv64_write_sscratch((uint64_t)(uintptr_t)&g_riscv64_trap_scratch);
}

void riscv64_timer_init(void) {
    riscv64_timer_arm_next();
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
        riscv64_trap_rearm_current_kernel_stack();
        return;
    }

    riscv64_trap_print_frame(frame);
    riscv64_wait_forever();
}
