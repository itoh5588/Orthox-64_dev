#include <stdint.h>
#include "riscv64/csr.h"
#include "riscv64/boot.h"
#include "riscv64/plic.h"
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
static uint64_t g_riscv64_last_timer_deadline;
/*
 * ワンショットログ用のフラグ。全 hart がタイマー割り込みを受けるので、
 * 「カウンタが 1 のときだけ出す」方式だと誰も 1 を観測できないことがある
 * (無ロックの ++ が競合する)。__atomic_exchange で最初の 1 人だけが 0 を
 * 受け取るようにして、ちょうど 1 回出す。
 */
static int g_riscv64_logged_first_timer;
static int g_riscv64_logged_timer_after_user_handoff;
static int g_riscv64_logged_first_uart_irq;

static int riscv64_log_once(int* flag) {
    return __atomic_exchange_n(flag, 1, __ATOMIC_RELAXED) == 0;
}

static void riscv64_trap_rearm_current_kernel_stack(void) {
    struct cpu_local* cpu = get_cpu_local();
    if (!cpu || !cpu->kernel_stack) return;
    // sscratch は riscv64_trap_init() で一度だけ設定し、以後は動かさない。
    // ここは「次にユーザーから来たときに張るカーネルスタック」の更新だけ
    riscv64_trap_scratch_self()->kernel_sp = cpu->kernel_stack;
}

/* 戻り値は SBI のエラー。全 hart が呼ぶのでグローバルに溜めず呼び出し元へ返す */
static long riscv64_timer_arm_next(void) {
    uint64_t now = riscv64_read_time();
    uint64_t deadline = now + g_riscv64_timer_interval;
    riscv64_sbi_ret_t ret = riscv64_sbi_set_timer(deadline);
    g_riscv64_last_timer_deadline = deadline;
    return ret.error;
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
    /* トラップ時にハードウェアが SIE を落とすので、syscall 処理の間は明示的に
     * 開け直す。ここを開けないと「カーネル内で待つ処理にはタイマーが届かない」
     * ままになる (wait4 の kernel_yield ループが典型)。
     * SUM はユーザー空間のアクセス許可。どちらも復帰時に元へ戻す。 */
    riscv64_write_sstatus(sstatus | RISCV64_SSTATUS_SUM | RISCV64_SSTATUS_SIE);
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
    riscv64_uart_puts("  tp     : 0x");
    riscv64_uart_puthex64(frame->tp);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  sp     : 0x");
    riscv64_uart_puthex64(frame->sp);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  ra     : 0x");
    riscv64_uart_puthex64(frame->ra);
    riscv64_uart_puts("\n");
    riscv64_uart_puts("  curtp  : 0x");
    riscv64_uart_puthex64(riscv64_current_hart_index());
    riscv64_uart_puts("\n");
}

void riscv64_trap_init(void) {
    struct riscv64_trap_scratch* scratch = riscv64_trap_scratch_self();
    scratch->hart_index = riscv64_current_hart_index();
    // sscratch は常に per-hart scratch を指す。ユーザー/カーネルの判定は
    // trap.S が sstatus.SPP で行うので、ここ以降 sscratch を動かしてはいけない
    riscv64_write_sscratch((uint64_t)(uintptr_t)scratch);
    riscv64_write_stvec((uint64_t)(uintptr_t)riscv64_trap_entry);
    // 副 hart のブートログは冗長なので boot hart のみ出す
    if (scratch->hart_index == 0) {
        riscv64_uart_puts("  trap vector installed\n");
    }
}

/* 次にユーザーモードからトラップしたときに張るカーネルスタックを登録する。
 * sscratch には触らない (触ると「カーネル実行中に sscratch が変わる」窓ができ、
 * かつては入口がそれをユーザー由来と誤判定してスタックを踏み潰していた) */
void riscv64_trap_set_kernel_stack(uint64_t kernel_sp) {
    struct riscv64_trap_scratch* scratch = riscv64_trap_scratch_self();
    scratch->hart_index = riscv64_current_hart_index();
    scratch->kernel_sp = kernel_sp;
}

/*
 * カーネル実行中も割り込みを取れるようにする (hart ごとに 1 回)。
 * stvec / sscratch / タイマーが揃ったあとに呼ぶこと。
 *
 * カーネルは非プリエンプティブのまま: riscv64_trap_dispatch() が
 * 「SPP==0 (ユーザーから来た) かつ BKL 非保持」のときしか切り替えない。
 * 割り込みハンドラ自身は SIE を開け直さないので、ネストは 1 段までに収まる。
 */
void riscv64_interrupts_enable(void) {
    /* resched IPI (supervisor software interrupt) は boot hart でも受ける。
     * 以前は副 hart 側だけで有効化しており、cpu 0 への IPI が届かなかった */
    riscv64_write_sie(riscv64_read_sie() | RISCV64_SIE_SSIE | RISCV64_SIE_SEIE);
    riscv64_write_sstatus(riscv64_read_sstatus() | RISCV64_SSTATUS_SIE);
}

void riscv64_timer_init(void) {
    long err;
    riscv64_write_sie(riscv64_read_sie() | RISCV64_SIE_STIE);
    err = riscv64_timer_arm_next();
    if (riscv64_current_hart_index() != 0) return;
    riscv64_uart_puts("  sbi timer armed\n");
    if (err != 0) {
        riscv64_uart_puts("  timer err: 0x");
        riscv64_uart_puthex64((uint64_t)err);
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

    if (frame->scause == RISCV64_SCAUSE_SEXT) {
        /* PLIC から 1 件受け取って処理し、完了を返す。
         * claim が 0 を返すのは他 hart に取られた場合 (何もしないのが正しい) */
        uint32_t irq = riscv64_plic_claim();
        if (irq == RISCV64_IRQ_UART0) {
            /* 受信 FIFO をリングへ移し、待っているタスクを起こす */
            riscv64_console_poll_input();
            if (riscv64_log_once(&g_riscv64_logged_first_uart_irq)) {
                riscv64_uart_puts("riscv64 uart rx interrupt\n");
            }
        }
        riscv64_plic_complete(irq);
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
        __atomic_fetch_add(&g_riscv64_timer_ticks, 1ULL, __ATOMIC_RELAXED);
        (void)riscv64_timer_arm_next();
        riscv64_handle_timer_interrupt();
        if (riscv64_log_once(&g_riscv64_logged_first_timer)) {
            riscv64_uart_puts("riscv64 supervisor timer interrupt\n");
        }
        if (riscv64_user_handoff_started() &&
            riscv64_log_once(&g_riscv64_logged_timer_after_user_handoff)) {
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
