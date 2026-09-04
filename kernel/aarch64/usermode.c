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
#include "linux_syscall.h"
#include "task.h"   /* get_current_task (S-1) */

/* SAVE_ALL が積んだフレームの並び。vectors.S と対で決まっている。
 *
 *   frame[0..29]  x0..x29
 *   frame[30]     x30 (lr)
 *   frame[31]     ELR_EL1   例外からの戻り先
 *   frame[32]     SPSR_EL1  戻り先の状態
 */
#define FRAME_X30    30
#define FRAME_ELR    31
#define FRAME_SPSR   32
#define FRAME_SP_EL0 33   /* ユーザーの sp。M3c-2a で SAVE_ALL に足した */

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

/* **落ちたユーザープロセスを exit と同じ道で終わらせる (S-1)。**戻らない。
 * 実体は kernel/linux_syscall.c */
void linux_task_kill_current(int status);

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
    /* SP_EL0 の探針 (M3c-2a)。**フレームに足しただけでは、それが
     * ハードウェアに効いている証拠にならない。** SP_EL0 は例外で自動的に
     * 保たれるので、save も restore も外して通ってしまう (逆確認で実証)。
     * そこでカーネル側から意図的に動かし、次の svc で戻ってくるかを見る */
    uint64_t sp_probe_saved;    /* 1 回目の svc で SAVE_ALL が積んでいた値 */
    uint64_t sp_probe_expect;
    int sp_probe_state;         /* 0 未実施 / 1 ずらした / 2 判定済み */
    int sp_probe_save_ok;       /* 積んだ値が真値と一致したか */
    int sp_probe_ok;
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

    /* ---- SP_EL0 の探針 --------------------------------------------------
     * **読みと書きを別々に確かめる。** 片方だけだと素通りする:
     *
     *   1 回目  SAVE_ALL が積んだ値が、**独立に分かっている真値** (ユーザーの
     *           スタック上端) と一致するか。ユーザーは sp を触らないので、
     *           最初の svc の時点では上端のままのはず。
     *           ここを「前に積んだ値」と比べる形にしていたら、SAVE_ALL から
     *           sp_el0 を外しても通ってしまった (逆確認で実証)。
     *           カーネルスタックの同じ枠に前回の値が残るため
     *   2 回目  1 回目でずらした値が返ってくるか。**RESTORE_ALL が
     *           ハードウェアに書き戻している証拠**
     *
     * ずらす量は 16 の倍数 (SCTLR_EL1.SA0 でスタック整列チェックが有効)。
     * 判定した後は元に戻すので、ユーザーからは何も起きていないように見える */
    if (st->sp_probe_state == 0) {
        st->sp_probe_saved = frame[FRAME_SP_EL0];
        st->sp_probe_save_ok = (st->sp_probe_saved == aarch64_vm_user_stack_top_va());
        st->sp_probe_expect = st->sp_probe_saved - 16;
        frame[FRAME_SP_EL0] -= 16;
        st->sp_probe_state = 1;
    } else if (st->sp_probe_state == 1) {
        st->sp_probe_ok = st->sp_probe_save_ok &&
                          (frame[FRAME_SP_EL0] == st->sp_probe_expect);
        frame[FRAME_SP_EL0] += 16;
        st->sp_probe_state = 2;
    }

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
        aarch64_uart_puts("  [EL1] unimplemented syscall: ");
        aarch64_uart_puthex64(nr);
        aarch64_uart_puts("\n");
        frame[0] = (uint64_t)-38;   /* -ENOSYS */
        break;
    }

    /* **svc の ELR_EL1 は svc の次の命令を指している。** データアボートの
     * ように自分で 4 を足してはいけない。足すと命令を 1 つ飛ばす */
}

/* ---- M-1 の探針: 落ちた EL0 フレームを丸ごと出す -------------------------
 *
 * 日報2026-08-24 §13 の間欠障害 (4 コアで 8 回中 2 回、fork の子で
 * `ELR = FAR = 0x3fffffefea` の命令アボート) を絞り込むためのもの。
 * **ELR だけでは分岐が決まらない。** 一度の再現で次を全部取る:
 *
 *   CPU     どのコアで落ちたか。CPU 0 でも出るなら分散とは無関係
 *   pid/comm exec の前か後か。`comm` が親と同じなら exec 前 (fork 直後)
 *   SPSR    eret 列の問題なら EL/DAIF がおかしい
 *   SP_EL0  ELR がスタック領域を指しているので、SP との位置関係が要る
 *   x30     「戻り先」が壊れているなら x30 も同じ値のはず
 *   x0-x29  fork の戻り値 (x0) が親の値のままか等
 *
 * **落ちるのは間欠なので、出力が混ざらないよう console を握ったまま出す。** */
static void aarch64_dump_bad_user_frame(const uint64_t* frame, uint64_t esr, uint64_t far) {
    struct task* cur = get_current_task();
    struct cpu_local* cl = get_cpu_local();
    uint64_t mpidr = 0, sctlr = 0, ttbr0 = 0, tcr = 0;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    __asm__ volatile("mrs %0, tcr_el1"   : "=r"(tcr));

    aarch64_console_begin();
    aarch64_uart_puts("\n*** aarch64 unexpected exception from EL0 ***\n");
    aarch64_uart_puts("  CPU : mpidr=");
    aarch64_uart_puthex64(mpidr);
    aarch64_uart_puts(" cpu_id=");
    aarch64_uart_putdec64(cl ? (uint64_t)cl->cpu_id : (uint64_t)-1);
    aarch64_uart_puts("\n  TASK: pid=");
    aarch64_uart_putdec64(cur ? (uint64_t)cur->pid : 0);
    aarch64_uart_puts(" ppid=");
    aarch64_uart_putdec64(cur ? (uint64_t)cur->ppid : 0);
    aarch64_uart_puts(" comm=");
    aarch64_uart_puts(cur ? cur->comm : "(none)");
    aarch64_uart_puts("\n  ESR : ");
    aarch64_uart_puthex64(esr);
    aarch64_uart_puts("\n  FAR : ");
    aarch64_uart_puthex64(far);
    aarch64_uart_puts("\n  ELR : ");
    aarch64_uart_puthex64(frame[FRAME_ELR]);
    aarch64_uart_puts("\n  SPSR: ");
    aarch64_uart_puthex64(frame[FRAME_SPSR]);
    aarch64_uart_puts("\n  SP0 : ");
    aarch64_uart_puthex64(frame[FRAME_SP_EL0]);
    aarch64_uart_puts("\n  x30 : ");
    aarch64_uart_puthex64(frame[FRAME_X30]);
    aarch64_uart_puts("\n  TTBR0: ");
    aarch64_uart_puthex64(ttbr0);
    aarch64_uart_puts(" SCTLR: ");
    aarch64_uart_puthex64(sctlr);
    aarch64_uart_puts(" TCR: ");
    aarch64_uart_puthex64(tcr);
    /* **タスクが持っている「あるべき値」も並べる。** frame の中身と
     * 突き合わせれば、壊れたのが frame かタスクかが分かる */
    if (cur) {
        aarch64_uart_puts("\n  want: entry=");
        aarch64_uart_puthex64(cur->user_entry);
        aarch64_uart_puts(" sp_top=");
        aarch64_uart_puthex64(cur->user_stack_top);
        aarch64_uart_puts(" sp=");
        aarch64_uart_puthex64(cur->user_stack);
        aarch64_uart_puts(" kstack_top=");
        aarch64_uart_puthex64(cur->kstack_top);
    }
    aarch64_uart_puts("\n  frame @ ");
    aarch64_uart_puthex64((uint64_t)(uintptr_t)frame);
    aarch64_uart_puts("\n");
    for (int i = 0; i < 30; i += 2) {
        aarch64_uart_puts("  x");
        aarch64_uart_putdec64((uint64_t)i);
        aarch64_uart_puts(i < 10 ? "  = " : " = ");
        aarch64_uart_puthex64(frame[i]);
        aarch64_uart_puts("   x");
        aarch64_uart_putdec64((uint64_t)(i + 1));
        aarch64_uart_puts((i + 1) < 10 ? "  = " : " = ");
        aarch64_uart_puthex64(frame[i + 1]);
        aarch64_uart_puts("\n");
    }
    aarch64_uart_puts("aarch64-user-BAD\n");
    aarch64_console_end();
}

/* 下位 EL (EL0) からの同期例外。svc と、ユーザーが起こしたアボートの両方が
 * ここに来る。ESR の EC で振り分ける */
void aarch64_lower_el_sync(uint64_t* frame, uint64_t esr, uint64_t far) {
    uint64_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    user_run_state_t* st = urun();

    if (ec == ESR_EC_SVC64) {
        /* **共有システムコール層へ渡す (P1)。**
         * M3a の自前 aarch64_svc は、共有層が来るまでの足場だった。
         * いまは kernel/linux_syscall.c が受ける (riscv64 と同じ実装) */
        if (aarch64_use_shared_syscalls()) {
            /* **syscall 処理の間だけ IRQ を開ける。**
             *
             * svc で例外に入った時点で、ハードウェアが PSTATE.{D,A,I,F} を
             * 全部 1 にする。閉じたまま処理すると、**カーネル内で「割り込みで
             * しか進まないもの」を待つ処理が永久に返らない。**
             *
             * P2 (musl) で踏んだ。open(O_CREAT) がディスク書き込みを伴い、
             * gdbstub で実測したところ:
             *
             *   PC     = vblk_rw+288   (wfi を挟む完了待ちループの中)
             *   PSTATE = 0x800003c5    -> bit7 (I) = 1 でマスク
             *   3 回サンプリングして PC / SP が 1 ビットも動かない
             *
             * あのループを抜ける道は「完了フラグが立つ」か「タイマの ticks が
             * タイムアウトを超える」の 2 つだけで、**どちらも
             * aarch64_irq_handler からしか進まない**。だから完了も
             * タイムアウトも来ない。open(O_RDONLY) が通っていたのは、
             * ディスク書き込みを伴わなかったため。
             *
             * riscv64 は kernel/riscv64/trap.c の riscv64_handle_ecall で
             * sstatus.SIE に対して同じことをしている。**元の値へ必ず戻す** */
            uint64_t daif;
            __asm__ volatile("mrs %0, daif" : "=r"(daif));
            __asm__ volatile("msr daifclr, #2" ::: "memory");   /* I だけ開ける */
            linux_syscall_dispatch((arch_syscall_frame_t*)frame);
            __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
            return;
        }
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

    /* ---- スタックの下端なら伸ばして、落ちた命令をやり直させる ----------
     *
     * DFSC が 0b0001xx なら変換フォールト (下位 2bit が段)。張っていない
     * ページを触ったということなので、スタックの範囲内かを task 側で見て
     * もらう。**ELR は進めない。**同じ命令をもう一度実行させる。
     *
     * 2026-08-30、GCC の cc1 が insn-attrtab.c で 252KiB を使い切って
     * ここに落ちた (ESR=0x92000047, FAR=0x3ffffbfe60)。 */
    if (ec == ESR_EC_DABT_LOW && ((esr & 0x3fULL) >> 2) == 0x1ULL) {
        struct task* cur = get_current_task();
        if (cur && cur->ppid != 0 && task_grow_user_stack(cur, far) == 0) return;
    }

    /* 想定外。**黙って戻らない。** ユーザーを再開させると同じ所で
     * 落ち続けて無限ループになる */
    aarch64_dump_bad_user_frame(frame, esr, far);
    st->exited = 1;
    st->exit_code = (uint64_t)-1;

    /* ---- S-1: タスクなら本当に殺す --------------------------------------
     *
     * **下の「戻り先の差し替え」は探針の経路にしか効かない。**
     * `aarch64_user_abort_landing` は `aarch64_enter_el0` が積んだ
     * callee-saved を降ろして呼び出し元へ戻る作りで、**そこから EL0 に
     * 入った場合しか成立しない。**
     *
     * `exec` したタスクは文脈切り替えから eret で EL0 に入るので、
     * 積まれたフレームが無い。**ゴミを pop して ret し、また同じ所で
     * 落ちる。**2026-08-22 の実機で、gcc が毎秒 58 回この例外を上げ続け、
     * Ctrl-C も効かず電源断でしか止まらなかった。
     *
     * **タスク文脈なら exit と同じ道を通す。**fd を閉じ、ゾンビにして、
     * 親に知らせる。この関数は戻らない */
    {
        struct task* cur = get_current_task();
        if (cur && cur->ppid != 0) {
            aarch64_console_begin();
            aarch64_uart_puts("  [EL1] crashed, so pid ");
            aarch64_uart_putdec64((uint64_t)cur->pid);
            aarch64_uart_puts(" terminating\n");
            aarch64_console_end();
            /* **割り込みを開けてから。**この先は kernel_yield まで進み、
             * 閉じたままだと切り替えが来ない */
            __asm__ volatile("msr daifclr, #2" ::: "memory");
            linux_task_kill_current(-1);   /* 戻らない */
        }
    }

    /* ここに来るのは探針の経路 (aarch64_enter_el0 から入った場合) だけ。
     * EL0 には戻さず、カーネルの続きへ抜ける */
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
    st->sp_probe_state = 0;
    st->sp_probe_save_ok = 0;
    st->sp_probe_ok = 0;
    st->sp_probe_saved = 0;

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
    aarch64_uart_puts(st->exit_code == 0 ? "  ok (private data)\n"
                                            : "  BAD (previous address space value is visible)\n");

    /* --- EL0 実行中に tick が入ったか -----------------------------------
     * **「下位 EL の IRQ」ベクタ (+0x480) が効いている証拠。** */
    aarch64_uart_puts("  el0 ticks : ");
    aarch64_uart_puthex64(ticks_after - ticks_before);
    aarch64_uart_puts((ticks_after - ticks_before) >= AARCH64_USER_MIN_TICKS
                      ? "  ok\n" : "  BAD (tick not delivered at EL0)\n");

    /* --- EL0 からカーネルのページを読んで落ちたか ------------------------ */
    aarch64_uart_puts("  user probe: ");
    if (st->fault_esr == 0) {
        aarch64_uart_puts("BAD (kernel .text was readable from EL0)\n");
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
            aarch64_uart_puts(" BAD (not a permission fault)\n");
        }
    }

    /* --- カーネルの VA を write に渡したときの戻り値 ---------------------
     * **-14 (-EFAULT) でなければ、カーネルの中身が漏れている。** */
    aarch64_uart_puts("  bad ptr   : ");
    aarch64_uart_puthex64(st->bad_ptr_ret);
    aarch64_uart_puts(st->bad_ptr_ret == (uint64_t)-14
                      ? "  ok (rejected with -EFAULT)\n"
                      : "  BAD (allowed reading kernel VA)\n");

    /* --- SP_EL0 がフレーム経由で効いているか (M3c-2a) --------------------
     * **execve / fork がユーザーのスタックを組み替える経路そのもの。**
     * ここが効いていないと、共有層は sp を差し替えたつもりで差し替わらない */
    aarch64_uart_puts("  sp probe  : ");
    aarch64_uart_puthex64(st->sp_probe_saved);
    aarch64_uart_puts(st->sp_probe_ok ? "  ok (SP_EL0 works via the frame)\n"
                                      : "  BAD (frame sp_el0 not propagated)\n");

    aarch64_console_end();

    return st->exited && st->exit_code == 0 &&
           (ticks_after - ticks_before) >= AARCH64_USER_MIN_TICKS &&
           st->fault_esr != 0 && st->bad_ptr_ret == (uint64_t)-14 &&
           st->sp_probe_ok && st->svc_count == 5;
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

    aarch64_uart_puts("--- M3a/M3b/M3c: EL0 + address space + context switch ---\n");
    aarch64_uart_puts("  user text : ");
    aarch64_uart_puthex64(aarch64_vm_user_entry_va());
    aarch64_uart_puts("\n  user sp   : ");
    aarch64_uart_puthex64(aarch64_vm_user_stack_top_va());
    aarch64_uart_puts("\n  probe tgt : ");
    aarch64_uart_puthex64(aarch64_user_probe_target());
    aarch64_uart_puts("  (kernel .text. must not be readable from EL0)\n");

    /* **先に EL1 で同じだけ空回しして比べる。** これをやらないと、EL0 で
     * tick が入らなかったときに「EL0 の IRQ ベクタが効いていない」のか
     * 「そもそもタイマが止まっている」のかが分けられない */
    {
        uint64_t t0 = aarch64_timer_ticks();
        for (volatile uint64_t i = 0; i < AARCH64_USER_SPIN; i++) { }
        aarch64_uart_puts("  el1 ticks : ");
        aarch64_uart_puthex64(aarch64_timer_ticks() - t0);
        aarch64_uart_puts("  (for comparison. spun the same loop at EL1)\n");
    }

    space_a = aarch64_vm_create_user_space();
    space_b = aarch64_vm_create_user_space();
    if (!space_a || !space_b || space_a == space_b) {
        aarch64_uart_puts("  could not create 2 address spaces\n");
        aarch64_uart_puts("aarch64-user-BAD\n");
        return 0;
    }

    aarch64_task_init();

    /* カーネルスレッド 2 本。譲り合ってカウンタを回す */
    k1 = aarch64_task_create(aarch64_kthread_body, 0);
    k2 = aarch64_task_create(aarch64_kthread_body, 0);

    /* ユーザータスク 2 本。**それぞれ別のアドレス空間** */
    g_task_label[3] = "space A";
    g_task_label[4] = "space B";
    u1 = aarch64_task_create(aarch64_utask_body, space_a);
    u2 = aarch64_task_create(aarch64_utask_body, space_b);

    if (k1 < 0 || k2 < 0 || u1 < 0 || u2 < 0) {
        aarch64_uart_puts("  could not create task\n");
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
                      ? "  ok (both ran to completion)\n" : "  BAD\n");
    if (aarch64_task_counter(k1) != 200 || aarch64_task_counter(k2) != 200) ok = 0;

    aarch64_uart_puts("  switches  : ");
    aarch64_uart_putdec64(aarch64_task_switch_count());
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
