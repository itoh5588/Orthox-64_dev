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
#include "aarch64/task.h"

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

/* ---- 実行ごとの状態は **タスクごとに持つ** (M3c-1) ----------------------
 *
 * M3b までは 1 本ずつ順番に走らせていたので単一のグローバルで足りていた。
 * **タスクにして並行にした瞬間に壊れた。** 2 つのユーザータスクが
 * 探針のフラグを取り合い、片方の fault がもう片方に消されて
 * 「想定外の例外」になった。
 *
 * 並行にすると、単一のグローバルは必ず壊れる。 */
typedef struct user_run_state {
    int      exited;
    uint64_t exit_code;
    uint64_t svc_count;
    uint64_t bad_ptr_ret;       /* 悪いポインタを渡したときの write の戻り値 */
    volatile int expect_fault;  /* 「触ったら落ちるはず」の探針 */
    volatile uint64_t fault_esr;
    volatile uint64_t fault_far;
} user_run_state_t;

static user_run_state_t g_urun[AARCH64_MAX_TASKS];

static user_run_state_t* urun(void) {
    aarch64_task_t* me = aarch64_task_current();
    int id = me ? me->id : 0;
    if (id < 0 || id >= AARCH64_MAX_TASKS) id = 0;
    return &g_urun[id];
}

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
    if (!aarch64_vm_user_range_ok(aarch64_task_current()->ttbr0, buf, len, 0)) {
        return -14;                         /* -EFAULT */
    }
    /* **1 回の write は割れない。** 別のユーザータスクの出力が途中に
     * 差し込まれると、行単位で読む側から見て別の内容になる。
     * (長さの上限は付けていない。EL0 が巨大な write を投げると、そのぶん
     *  切り替えが遅れる。共有システムコール層に載せるときに区切ること) */
    aarch64_console_begin();
    for (i = 0; i < len; i++) {
        if (p[i] == '\n') aarch64_uart_putchar('\r');
        aarch64_uart_putchar(p[i]);
    }
    aarch64_console_end();
    return (int64_t)len;
}

static void aarch64_svc(uint64_t* frame) {
    uint64_t nr = frame[8];      /* x8 = システムコール番号 */
    user_run_state_t* st = urun();

    st->svc_count++;

    switch (nr) {
    case AARCH64_NR_WRITE:
        frame[0] = (uint64_t)sys_write(frame[0], frame[1], frame[2]);
        /* カーネルの VA を渡された回の戻り値を取っておく */
        if ((int64_t)frame[0] < 0) st->bad_ptr_ret = frame[0];
        break;
    case AARCH64_NR_EXIT:
        st->exited = 1;
        st->exit_code = frame[0];
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
    user_run_state_t* st = urun();

    if (ec == ESR_EC_SVC64) {
        aarch64_svc(frame);
        if (st->exited) {
            /* exit されたので EL0 へは戻さない。戻り先を差し替えて
             * カーネルの続きへ抜ける */
            frame[FRAME_ELR] = (uint64_t)(uintptr_t)aarch64_user_abort_landing;
            frame[FRAME_SPSR] = SPSR_EL1H;
        }
        return;
    }

    if ((ec == ESR_EC_DABT_LOW || ec == ESR_EC_IABT_LOW) && st->expect_fault) {
        /* 想定内。記録して、落ちた命令の次から再開させる */
        st->expect_fault = 0;
        st->fault_esr = esr;
        st->fault_far = far;
        frame[FRAME_ELR] += 4;
        return;
    }

    /* 想定外。**黙って戻らない。** ユーザーを再開させると同じ所で
     * 落ち続けて無限ループになる */
    aarch64_console_begin();
    aarch64_uart_puts("\n*** aarch64 EL0 からの想定外の例外 ***\n");
    aarch64_uart_puts("  ESR : ");
    aarch64_uart_puthex64(esr);
    aarch64_uart_puts("\n  FAR : ");
    aarch64_uart_puthex64(far);
    aarch64_uart_puts("\n  ELR : ");
    aarch64_uart_puthex64(frame[FRAME_ELR]);
    aarch64_uart_puts("\naarch64-user-BAD\n");
    aarch64_console_end();
    st->exited = 1;
    st->exit_code = (uint64_t)-1;
    /* EL0 には戻さず、カーネルの続きへ抜ける */
    frame[FRAME_ELR] = (uint64_t)(uintptr_t)aarch64_user_abort_landing;
    frame[FRAME_SPSR] = SPSR_EL1H;
}

/* 1 つのアドレス空間でユーザーを 1 回走らせる。
 * 戻り値: 成功なら 1 */
int aarch64_user_run_once(const char* label, uint64_t root_pa) {
    uint64_t ticks_before, ticks_after;
    user_run_state_t* st = urun();

    /* **複数回の呼び出しで 1 行を組むので、明示的に囲む** */
    aarch64_console_begin();
    aarch64_uart_puts("  --- ");
    aarch64_uart_puts(label);
    aarch64_uart_puts(" ttbr0=");
    aarch64_uart_puthex64(root_pa);
    aarch64_uart_puts("\n");
    aarch64_console_end();

    /* **タスクとして走る場合、TTBR0 は切り替えのときに入れ替わっている。**
     * ここで入れ直しても同じ値になるだけだが、単体で呼ばれたときのために残す */
    aarch64_vm_switch_user_space(root_pa);

    st->exited = 0;
    st->svc_count = 0;
    st->fault_esr = 0;
    st->bad_ptr_ret = 0;
    st->expect_fault = 1;

    ticks_before = aarch64_timer_ticks();
    aarch64_enter_el0(aarch64_vm_user_entry_va(),
                      aarch64_vm_user_stack_top_va(),
                      aarch64_user_probe_target());
    ticks_after = aarch64_timer_ticks();

    /* **報告はまとめて 1 単位にする。** EL0 を走らせているあいだは切り替えを
     * 止めていない (止めると el0 ticks の判定が意味を失う)。止めるのは
     * 戻ってきてから報告を出しおえるまでの、この区間だけ */
    aarch64_console_begin();

    aarch64_uart_puts("  svc calls : ");
    aarch64_uart_puthex64(st->svc_count);
    aarch64_uart_puts("  marker    : ");
    aarch64_uart_puthex64(st->exit_code);
    aarch64_uart_puts(st->exit_code == 0 ? "  ok (私物のデータ)\n"
                                            : "  BAD (前の空間の値が見えている)\n");

    /* --- EL0 実行中に tick が入ったか -----------------------------------
     * **「下位 EL の IRQ」ベクタ (+0x480) が効いている証拠。** */
    aarch64_uart_puts("  el0 ticks : ");
    aarch64_uart_puthex64(ticks_after - ticks_before);
    aarch64_uart_puts((ticks_after - ticks_before) >= AARCH64_USER_MIN_TICKS
                      ? "  ok\n" : "  BAD (EL0 で tick が入らない)\n");

    /* --- EL0 からカーネルのページを読んで落ちたか ------------------------ */
    aarch64_uart_puts("  user probe: ");
    if (st->fault_esr == 0) {
        aarch64_uart_puts("BAD (EL0 からカーネルの .text が読めた)\n");
    } else {
        uint64_t ec = (st->fault_esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
        uint64_t dfsc = st->fault_esr & ESR_DFSC_MASK;
        aarch64_uart_puts("ESR=");
        aarch64_uart_puthex64(st->fault_esr);
        aarch64_uart_puts(" FAR=");
        aarch64_uart_puthex64(st->fault_far);
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
    aarch64_uart_puthex64(st->bad_ptr_ret);
    aarch64_uart_puts(st->bad_ptr_ret == (uint64_t)-14
                      ? "  ok (-EFAULT で弾いた)\n"
                      : "  BAD (カーネルの VA を読ませてしまった)\n");

    aarch64_console_end();

    return st->exited && st->exit_code == 0 &&
           (ticks_after - ticks_before) >= AARCH64_USER_MIN_TICKS &&
           st->fault_esr != 0 && st->bad_ptr_ret == (uint64_t)-14 &&
           st->svc_count == 5;
}

/* ---- M3c-1: タスクとして走らせる ---------------------------------------
 *
 * ここまでは aarch64_user_run が EL0 を「呼び出して戻る」形だった。
 * M3c-1 では**タスクにして、タイマ割り込みで切り替えながら**走らせる。 */
static const char* g_task_label[AARCH64_MAX_TASKS];
static int g_task_ok[AARCH64_MAX_TASKS];

/* カーネルスレッド。譲りながらカウンタを回すだけ */
static void aarch64_kthread_body(void) {
    aarch64_task_t* me = aarch64_task_current();
    for (int i = 0; i < 200; i++) {
        me->counter++;
        aarch64_task_yield();
    }
}

/* ユーザータスク。自分のアドレス空間で EL0 を走らせる */
static void aarch64_utask_body(void) {
    aarch64_task_t* me = aarch64_task_current();
    g_task_ok[me->id] = aarch64_user_run_once(g_task_label[me->id], me->ttbr0);
    me->counter++;
}

int aarch64_user_run(void) {
    uint64_t space_a, space_b;
    int k1, k2, u1, u2;
    int ok = 1;

    aarch64_uart_puts("--- M3a/M3b/M3c: EL0 + アドレス空間 + コンテキストスイッチ ---\n");
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

    space_a = aarch64_vm_create_user_space();
    space_b = aarch64_vm_create_user_space();
    if (!space_a || !space_b || space_a == space_b) {
        aarch64_uart_puts("  アドレス空間を 2 つ作れなかった\n");
        aarch64_uart_puts("aarch64-user-BAD\n");
        return 0;
    }

    aarch64_task_init();

    /* カーネルスレッド 2 本。譲り合ってカウンタを回す */
    k1 = aarch64_task_create(aarch64_kthread_body, 0);
    k2 = aarch64_task_create(aarch64_kthread_body, 0);

    /* ユーザータスク 2 本。**それぞれ別のアドレス空間** */
    g_task_label[3] = "空間 A";
    g_task_label[4] = "空間 B";
    u1 = aarch64_task_create(aarch64_utask_body, space_a);
    u2 = aarch64_task_create(aarch64_utask_body, space_b);

    if (k1 < 0 || k2 < 0 || u1 < 0 || u2 < 0) {
        aarch64_uart_puts("  タスクを作れなかった\n");
        aarch64_uart_puts("aarch64-user-BAD\n");
        return 0;
    }

    /* 全部終わるまで譲り続ける。**自分 (0 番) もタスクの 1 つ** */
    while (aarch64_task_state(k1) != AARCH64_TASK_DONE ||
           aarch64_task_state(k2) != AARCH64_TASK_DONE ||
           aarch64_task_state(u1) != AARCH64_TASK_DONE ||
           aarch64_task_state(u2) != AARCH64_TASK_DONE) {
        aarch64_task_yield();
    }

    aarch64_uart_puts("  kthreads  : ");
    aarch64_uart_puthex64(aarch64_task_counter(k1));
    aarch64_uart_puts(" / ");
    aarch64_uart_puthex64(aarch64_task_counter(k2));
    aarch64_uart_puts(aarch64_task_counter(k1) == 200 && aarch64_task_counter(k2) == 200
                      ? "  ok (両方最後まで回った)\n" : "  BAD\n");
    if (aarch64_task_counter(k1) != 200 || aarch64_task_counter(k2) != 200) ok = 0;

    aarch64_uart_puts("  switches  : ");
    aarch64_uart_puthex64(aarch64_task_switch_count());
    aarch64_uart_puts(aarch64_task_switch_count() > 0 ? "  ok\n" : "  BAD\n");
    if (aarch64_task_switch_count() == 0) ok = 0;

    if (!g_task_ok[u1] || !g_task_ok[u2]) ok = 0;

    if (ok) {
        aarch64_uart_puts("aarch64-user-ok\n");
        return 1;
    }
    aarch64_uart_puts("aarch64-user-BAD\n");
    return 0;
}
