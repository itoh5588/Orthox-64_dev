/*
 * EL0 への遷移と svc の入口 (M3a)。
 *
 * riscv64 との対応:
 *
 *   |             | riscv64        | AArch64                       |
 *   |-------------|----------------|-------------------------------|
 *   | 特権を降りる | sret           | eret                          |
 *   | 戻り先       | sepc           | ELR_EL1                       |
 *   | 戻り先の状態 | sstatus.SPP=0  | SPSR_EL1 = 0 (EL0t)           |
 *   | ユーザー sp  | 同じ sp        | **SP_EL0 (別レジスタ)**       |
 *   | システムコール| ecall          | svc #0                        |
 *   | 番号         | a7             | x8                            |
 *
 * **sp が別レジスタなのが一番の違い。** EL1 は SP_EL1、EL0 は SP_EL0 を使う。
 * 例外で EL1 に入ると CPU が自動的に SP_EL1 へ切り替えるので、カーネルの
 * スタックがユーザーに壊される心配が無い。riscv64 で sscratch を使って
 * 手で入れ替えていた所が、ハードウェアで済む。
 *
 * このマイルストーンではアドレス空間を分けていない。ユーザーのページは
 * カーネルと同じテーブルに、**AP (権限ビット) だけを変えて**張ってある。
 * 分けるには TTBR1 でカーネルを上位半分へ移すのが先で、それは M3b。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/usermode.h"
#include "aarch64/vm.h"

/* SAVE_ALL が積んだフレームの並び。vectors.S と対で決まっている。
 *
 *   frame[0..29]  x0..x29
 *   frame[30]     x30 (lr)
 *   frame[31]     ELR_EL1   例外からの戻り先
 *   frame[32]     SPSR_EL1  戻り先の状態
 */
#define FRAME_X30   30
#define FRAME_ELR   31
#define FRAME_SPSR  32

/* ESR_EL1 の Exception Class */
#define ESR_EC_SHIFT    26
#define ESR_EC_MASK     0x3fULL
#define ESR_EC_SVC64    0x15    /* AArch64 の svc 命令 */
#define ESR_EC_DABT_LOW 0x24    /* 下位 EL からのデータアボート */
#define ESR_EC_IABT_LOW 0x20    /* 下位 EL からの命令アボート */
#define ESR_DFSC_MASK   0x3fULL


/* EL1h (SP_EL1 を使う EL1) に戻るときの SPSR。DAIF は開けたまま。
 * EL0 には割り込みを開けて降りているので、戻るときも揃える */
#define SPSR_EL1H   0x00000005ULL

/* EL0 の入口とスタックの VA。**TTBR0 の中の VA で、カーネルの配置とは
 * 無関係** (M3b)。M3a ではカーネルと同じテーブルの VA だった */
uint64_t aarch64_vm_user_entry_va(void);
uint64_t aarch64_vm_user_stack_top_va(void);
uint64_t aarch64_vm_create_user_space(void);
void aarch64_vm_switch_user_space(uint64_t root_pa);
uint64_t aarch64_vm_user_root_pa(void);
int aarch64_vm_user_range_ok(uint64_t root_pa, uint64_t va, uint64_t len, int write);

uint64_t aarch64_timer_ticks(void);

/* EL0 へ降りる (user_blob.S と対になる entry.S 側の実装) */
void aarch64_enter_el0(uint64_t entry, uint64_t user_sp, uint64_t arg0);

/* EL0 を終えたときの着地点。aarch64_enter_el0 から見ると「戻ってきた」
 * ことになる。例外ハンドラが戻り先をここに差し替えて eret する */
void aarch64_user_abort_landing(void);

static int g_user_exited;
static uint64_t g_user_exit_code;
static uint64_t g_svc_count;
static uint64_t g_bad_ptr_ret;   /* 悪いポインタを渡したときの write の戻り値 */

/* 「EL0 からカーネルのページを触ったら落ちるはず」の探針。
 * M2 の MMU 探針と同じ形で、想定内なら命令 1 つぶん進めて再開させる */
static volatile int g_expect_user_fault;
static volatile uint64_t g_user_fault_esr;
static volatile uint64_t g_user_fault_far;

/* 触らせるカーネルのアドレス。カーネルの .text の先頭。
 * **張ってはあるが AP は EL1 だけ**なので、EL0 から読むと permission fault
 * になるはず。「張っていないから落ちる」のでは意味が無い (それは M2 で
 * 確かめた translation fault と同じことになってしまう) */
extern char __text_start[];

uint64_t aarch64_user_probe_target(void) {
    return (uint64_t)(uintptr_t)__text_start;
}

/* ---- システムコール ------------------------------------------------------
 *
 * M3a で実装するのは write と exit だけ。共有のシステムコール層を乗せるのは
 * M3c (riscv64 の kernel/riscv64/syscall.c は 1379 行ある)。 */
/* **ユーザーのポインタは必ず検査する (M3b-2)。**
 * アドレス空間を分けたので「いまのプロセスの TTBR0 に、EL0 が読める形で
 * 張られているか」をテーブルを歩いて確かめられるようになった。
 *
 * 検査しないと、ユーザーがカーネルの VA を渡すだけでカーネルの中身が
 * そのまま UART に出る。**アドレス空間を分けただけでは防げない**
 * (カーネル自身は上位 VA を読めてしまうため) */
static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    const char* p = (const char*)(uintptr_t)buf;
    uint64_t i;
    if (fd != 1 && fd != 2) return -9;      /* -EBADF */
    if (!aarch64_vm_user_range_ok(aarch64_vm_user_root_pa(), buf, len, 0)) {
        return -14;                         /* -EFAULT */
    }
    for (i = 0; i < len; i++) {
        if (p[i] == '\n') aarch64_uart_putchar('\r');
        aarch64_uart_putchar(p[i]);
    }
    return (int64_t)len;
}

static void aarch64_svc(uint64_t* frame) {
    uint64_t nr = frame[8];      /* x8 = システムコール番号 */

    g_svc_count++;

    switch (nr) {
    case AARCH64_NR_WRITE:
        frame[0] = (uint64_t)sys_write(frame[0], frame[1], frame[2]);
        /* カーネルの VA を渡された回 (4 回目) の戻り値を取っておく */
        if ((int64_t)frame[0] < 0) g_bad_ptr_ret = frame[0];
        break;
    case AARCH64_NR_EXIT:
        g_user_exited = 1;
        g_user_exit_code = frame[0];
        break;
    default:
        aarch64_uart_puts("  [EL1] 未実装のシステムコール: ");
        aarch64_uart_puthex64(nr);
        aarch64_uart_puts("\n");
        frame[0] = (uint64_t)-38;   /* -ENOSYS */
        break;
    }

    /* **svc の ELR_EL1 は svc の次の命令を指している。** データアボートの
     * ように自分で 4 を足してはいけない。足すと命令を 1 つ飛ばす */
}

/* 下位 EL (EL0) からの同期例外。svc と、ユーザーが起こしたアボートの両方が
 * ここに来る。ESR の EC で振り分ける */
void aarch64_lower_el_sync(uint64_t* frame, uint64_t esr, uint64_t far) {
    uint64_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;

    if (ec == ESR_EC_SVC64) {
        aarch64_svc(frame);
        if (g_user_exited) {
            /* exit されたので EL0 へは戻さない。戻り先を差し替えて
             * カーネルの続きへ抜ける */
            frame[FRAME_ELR] = (uint64_t)(uintptr_t)aarch64_user_abort_landing;
            frame[FRAME_SPSR] = SPSR_EL1H;
        }
        return;
    }

    if ((ec == ESR_EC_DABT_LOW || ec == ESR_EC_IABT_LOW) && g_expect_user_fault) {
        /* 想定内。記録して、落ちた命令の次から再開させる */
        g_expect_user_fault = 0;
        g_user_fault_esr = esr;
        g_user_fault_far = far;
        frame[FRAME_ELR] += 4;
        return;
    }

    /* 想定外。**黙って戻らない。** ユーザーを再開させると同じ所で
     * 落ち続けて無限ループになる */
    aarch64_uart_puts("\n*** aarch64 EL0 からの想定外の例外 ***\n");
    aarch64_uart_puts("  ESR : ");
    aarch64_uart_puthex64(esr);
    aarch64_uart_puts("\n  FAR : ");
    aarch64_uart_puthex64(far);
    aarch64_uart_puts("\n  ELR : ");
    aarch64_uart_puthex64(frame[FRAME_ELR]);
    aarch64_uart_puts("\naarch64-user-BAD\n");
    g_user_exited = 1;
    g_user_exit_code = (uint64_t)-1;
    /* EL0 には戻さず、カーネルの続きへ抜ける */
    frame[FRAME_ELR] = (uint64_t)(uintptr_t)aarch64_user_abort_landing;
    frame[FRAME_SPSR] = SPSR_EL1H;
}

/* 1 つのアドレス空間でユーザーを 1 回走らせる。
 * 戻り値: 成功なら 1 */
static int aarch64_user_run_once(const char* label, uint64_t root_pa) {
    uint64_t ticks_before, ticks_after;

    aarch64_uart_puts("  --- ");
    aarch64_uart_puts(label);
    aarch64_uart_puts(" ttbr0=");
    aarch64_uart_puthex64(root_pa);
    aarch64_uart_puts("\n");

    aarch64_vm_switch_user_space(root_pa);

    g_user_exited = 0;
    g_svc_count = 0;
    g_user_fault_esr = 0;
    g_bad_ptr_ret = 0;
    g_expect_user_fault = 1;

    ticks_before = aarch64_timer_ticks();
    aarch64_enter_el0(aarch64_vm_user_entry_va(),
                      aarch64_vm_user_stack_top_va(),
                      aarch64_user_probe_target());
    ticks_after = aarch64_timer_ticks();

    aarch64_uart_puts("  svc calls : ");
    aarch64_uart_puthex64(g_svc_count);
    aarch64_uart_puts("  marker    : ");
    aarch64_uart_puthex64(g_user_exit_code);
    aarch64_uart_puts(g_user_exit_code == 0 ? "  ok (私物のデータ)\n"
                                            : "  BAD (前の空間の値が見えている)\n");

    /* --- EL0 実行中に tick が入ったか -----------------------------------
     * **「下位 EL の IRQ」ベクタ (+0x480) が効いている証拠。** */
    aarch64_uart_puts("  el0 ticks : ");
    aarch64_uart_puthex64(ticks_after - ticks_before);
    aarch64_uart_puts((ticks_after - ticks_before) >= AARCH64_USER_MIN_TICKS
                      ? "  ok\n" : "  BAD (EL0 で tick が入らない)\n");

    /* --- EL0 からカーネルのページを読んで落ちたか ------------------------ */
    aarch64_uart_puts("  user probe: ");
    if (g_user_fault_esr == 0) {
        aarch64_uart_puts("BAD (EL0 からカーネルの .text が読めた)\n");
    } else {
        uint64_t ec = (g_user_fault_esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
        uint64_t dfsc = g_user_fault_esr & ESR_DFSC_MASK;
        aarch64_uart_puts("ESR=");
        aarch64_uart_puthex64(g_user_fault_esr);
        aarch64_uart_puts(" FAR=");
        aarch64_uart_puthex64(g_user_fault_far);
        /* DFSC が 0b0011xx なら permission fault。**translation fault
         * (0b0001xx) では駄目。** それは「張っていない」だけで、
         * 権限で弾いた証拠にならない */
        if (ec == ESR_EC_DABT_LOW && (dfsc >> 2) == 0x3ULL) {
            aarch64_uart_puts(" (permission fault) ok\n");
        } else {
            aarch64_uart_puts(" BAD (permission fault ではない)\n");
        }
    }

    /* --- カーネルの VA を write に渡したときの戻り値 ---------------------
     * **-14 (-EFAULT) でなければ、カーネルの中身が漏れている。** */
    aarch64_uart_puts("  bad ptr   : ");
    aarch64_uart_puthex64(g_bad_ptr_ret);
    aarch64_uart_puts(g_bad_ptr_ret == (uint64_t)-14
                      ? "  ok (-EFAULT で弾いた)\n"
                      : "  BAD (カーネルの VA を読ませてしまった)\n");

    return g_user_exited && g_user_exit_code == 0 &&
           (ticks_after - ticks_before) >= AARCH64_USER_MIN_TICKS &&
           g_user_fault_esr != 0 && g_bad_ptr_ret == (uint64_t)-14 &&
           g_svc_count == 5;
}

int aarch64_user_run(void) {
    uint64_t space_a, space_b;
    int ok;

    aarch64_uart_puts("--- M3a/M3b: EL0 + svc + プロセスごとのアドレス空間 ---\n");
    aarch64_uart_puts("  user text : ");
    aarch64_uart_puthex64(aarch64_vm_user_entry_va());
    aarch64_uart_puts("\n  user sp   : ");
    aarch64_uart_puthex64(aarch64_vm_user_stack_top_va());
    aarch64_uart_puts("\n  probe tgt : ");
    aarch64_uart_puthex64(aarch64_user_probe_target());
    aarch64_uart_puts("  (カーネルの .text。EL0 から読めてはいけない)\n");

    /* **先に EL1 で同じだけ空回しして比べる。** これをやらないと、EL0 で
     * tick が入らなかったときに「EL0 の IRQ ベクタが効いていない」のか
     * 「そもそもタイマが止まっている」のかが分けられない */
    {
        uint64_t t0 = aarch64_timer_ticks();
        for (volatile uint64_t i = 0; i < AARCH64_USER_SPIN; i++) { }
        aarch64_uart_puts("  el1 ticks : ");
        aarch64_uart_puthex64(aarch64_timer_ticks() - t0);
        aarch64_uart_puts("  (比較用。EL1 で同じだけ空回しした)\n");
    }

    /* **同じプログラムを別々のアドレス空間で 2 回走らせる。**
     * テキストは共有、データは私物。2 回目も marker が 0 で読めれば、
     * データが本当に分離されている */
    space_a = aarch64_vm_create_user_space();
    space_b = aarch64_vm_create_user_space();
    if (!space_a || !space_b || space_a == space_b) {
        aarch64_uart_puts("  アドレス空間を 2 つ作れなかった\n");
        aarch64_uart_puts("aarch64-user-BAD\n");
        return 0;
    }

    ok  = aarch64_user_run_once("空間 A", space_a);
    ok &= aarch64_user_run_once("空間 B", space_b);

    if (ok) {
        aarch64_uart_puts("aarch64-user-ok\n");
        return 1;
    }
    aarch64_uart_puts("aarch64-user-BAD\n");
    return 0;
}
