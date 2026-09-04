/*
 * generic timer。riscv64 が SBI 経由で次回時刻を設定していたところを、
 * AArch64 は専用のシステムレジスタで行う。
 *
 *   CNTFRQ_EL0     周波数 (Hz)。QEMU virt では 62.5MHz
 *   CNTPCT_EL0     現在のカウンタ値 (単調増加)
 *   CNTP_TVAL_EL0  「あと何カウントで割り込むか」。書くと減算が始まる
 *   CNTP_CTL_EL0   bit0 有効 / bit1 マスク / bit2 発火中
 *
 * CVAL (絶対時刻) ではなく TVAL (相対) を使う。tick ごとに間隔を書き直す
 * だけで済み、riscv64 の「現在時刻 + 間隔を書く」より 1 手少ない。
 *
 * 割り込み番号は DTB の timer ノードの interrupts から取る (M2b)。
 * QEMU virt では <1 14 0x104> = PPI 14 → INTID 30。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "task.h"
#include "aarch64/trap.h"

#define TICK_HZ 100     /* 10ms 刻み。x86 / riscv64 の SCHED_TICK_MS と揃える */

#define CNTP_CTL_ENABLE  (1U << 0)
#define CNTP_CTL_IMASK   (1U << 1)

static uint64_t g_interval;
static volatile uint64_t g_ticks;

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_tval(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

static inline void write_ctl(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

uint64_t aarch64_timer_freq(void) {
    return read_cntfrq();
}

uint64_t aarch64_timer_ticks(void) {
    return g_ticks;
}

/* 非セキュア物理タイマの INTID。GIC に有効化を頼むときに使う */
uint32_t aarch64_timer_intid(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (b->timer_intid) return b->timer_intid;
    return AARCH64_TIMER_PPI_DEFAULT + AARCH64_PPI_INTID_BASE;
}

void aarch64_timer_init(void) {
    uint64_t freq = read_cntfrq();
    /* 周波数が 0 なら間隔が 0 になり、割り込みが際限なく上がって固まる。
     * 0 のときは既定値に退く (QEMU virt では 62.5MHz が入っている) */
    if (freq == 0) freq = 62500000UL;
    g_interval = freq / TICK_HZ;
    if (g_interval == 0) g_interval = 1;

    g_ticks = 0;
    write_tval(g_interval);
    write_ctl(CNTP_CTL_ENABLE);   /* IMASK は立てない = 割り込みを通す */
}

/* タイマ割り込みが上がったときに呼ぶ。次回ぶんを仕込み直す */
void aarch64_task_on_tick(void);

void aarch64_kbd_tick(void);

/* **鳴り終わったブロックを無音に戻す (D-3)。**PWM の FIFO を DMA が
 * 円環で埋め続けるので、戻さないと一周して同じ音がもう一度鳴る。
 * 1 ブロック 32ms に対しここは 10ms ごとなので取りこぼさない。
 * **音を持たない機械では何もしない** */
void sound_tick(void);

/* この CPU の番号。**記帳ではなく mpidr をそのまま読む** —
 * cpu_local が未設置の区間でも成立する (smp.c:98 と同じ形) */
static inline uint32_t timer_cpu_index(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xffULL);
}

/* ---- CPU ごとの稼働を数える (M-4 の切り分け) -----------------------------
 *
 * **「4 コアにしたのに縮まない」の原因を絞るための計器。**
 *
 * 実測 (2026-08-26): 実機でセルフホストのビルドを make -j4 で回したら
 * 50 分 9 秒。1 コアだった 8/23 の 52 分とほぼ同じで、**4 コアにした
 * 効果が出なかった。** ログからは「4 本並列で 50 分」と「逐次で 50 分」
 * が区別できない — 起動行に時刻が無いので、どちらでも同じに見える。
 *
 * **区別するには「各 CPU が実際に走っていた割合」を測るしかない。**
 *
 *   どのコアも高い    -> CPU は使い切っている。遅いのは別の所
 *                        (SD の I/O が本命。emmc2 は PIO のみで DMA 無し)
 *   cpu0 だけ高い     -> タスクが分散していない。スケジューラの問題
 *   どのコアも低い    -> 全員が何かを待っている
 *
 * タイマは PPI で**全 CPU に上がる**ので、自分の分は自分で数えられる。
 * tick ごとに加算 2 回だけなので、常時入れておいても割に合う。
 *
 * idle タスクを走らせていた tick は「暇」、それ以外は「働いた」。 */
static volatile uint64_t g_cpu_tick_all[AARCH64_MAX_CPUS];
static volatile uint64_t g_cpu_tick_busy[AARCH64_MAX_CPUS];
static volatile uint32_t g_cpu_runq_peak[AARCH64_MAX_CPUS];

/* ---- 誰が、どこを回っているか (G-1 / G-2 / P-2) --------------------------
 *
 * **`[cpu]` は「焼けている」ことしか言わない。**日報2026-08-29 §26 で
 * 2 コアが 100%、SD I/O ゼロのまま 74 分止まったとき、分かったのはそこまで
 * だった。**誰がどこを回っているかが分からないと、次に何を直すか決まらない。**
 *
 * タイマ割り込みは 10ms ごとに全コアへ上がる。そのときの `ELR_EL1` には
 * **中断した地点の PC がそのまま入っている** (vectors.S の SAVE_ALL は
 * 積むだけで書き替えない)。これを毎 tick 拾って多い順に数えれば、
 * 追加の割り込みも入力の口も要らない、ただの標本化プロファイラになる。
 *
 * 1 標本だけだと「たまたまそこ」を掴むので、**64 バイト単位に丸めて
 * 4 枠の多数決**を取る。区間で最も多かった枠と、その割合を出す。
 *
 * PC の上位が 0xffffff8000... ならカーネル、低ければ EL0 (ユーザー)。
 * out/kernel-aarch64.elf か走っているプログラムの符号表で引ける */
void aarch64_uart_puthex64(uint64_t v);

#define PC_SLOTS 4
/* **64 では粗すぎた。**spin_lock_irqsave の末尾・spin_unlock_irqrestore・
 * kernel_lock_enter の 3 つが 1 つの桶に入り、どこを指しているのか
 * 分からなくなった (2026-08-29 に読み違えた)。関数が混ざらない粒度にする */
#define PC_GRAIN 16ULL

struct pc_slot { uint64_t pc; uint32_t hits; };
static struct pc_slot g_pc[AARCH64_MAX_CPUS][PC_SLOTS];
static volatile uint32_t g_pc_total[AARCH64_MAX_CPUS];
static volatile int      g_cpu_pid[AARCH64_MAX_CPUS];
/* **呼び出し元。**IRQ を落としている区間は割り込みが入らないので、
 * 溜まった tick は「IRQ を開け直す場所」(spin_unlock_irqrestore) に
 * 集中する。PC だけでは区間の中身が見えないので、x30 で誰が開けたかを見る */
static volatile uint64_t g_cpu_lr[AARCH64_MAX_CPUS];
static char              g_cpu_comm[AARCH64_MAX_CPUS][16];

/* 自分のコアの枠だけを触るのでロックは要らない */
static void pc_sample(uint32_t cpu, const struct task *t,
                      const struct aarch64_trap_frame *f) {
    uint64_t pc;
    uint32_t i, worst = 0;
    int j;

    /* **フレームから採る。**mrs elr_el1 でも同じだが、同じフレームから
     * 呼び出し元 (x30) も採りたいので揃える */
    pc = f ? f->elr : 0;
    g_cpu_lr[cpu] = f ? f->x[30] : 0;
    pc &= ~(PC_GRAIN - 1ULL);

    /* **誰かは毎回書き換える。**枠に当たったときだけ書いていたころは、
     * 最初にその番地を踏んだタスクの名前が残り続けた (comm がまだ
     * 空の時期に掴むと "?" のまま貼り付く) */
    g_cpu_pid[cpu] = t ? t->pid : -1;
    for (j = 0; j < 15; j++) {
        char c = (t && t->comm[j]) ? t->comm[j] : '\0';
        g_cpu_comm[cpu][j] = c;
        if (!c) break;
    }
    g_cpu_comm[cpu][15] = '\0';

    g_pc_total[cpu]++;
    for (i = 0; i < PC_SLOTS; i++) {
        if (g_pc[cpu][i].hits && g_pc[cpu][i].pc == pc) { g_pc[cpu][i].hits++; return; }
        if (g_pc[cpu][i].hits < g_pc[cpu][worst].hits) worst = i;
    }
    /* 空きか、いちばん少ない枠を置き換える */
    g_pc[cpu][worst].pc = pc;
    g_pc[cpu][worst].hits = 1;
}

/* ---- タスク一覧 (G-1) ---------------------------------------------------
 *
 * **[pc] は走っているタスクしか映さない。**2026-08-30 のハングでは、
 * 親 2 人が wait4 で空回りしている一方、肝心の「終わらない子」は
 * 眠っていて 1 度も標本に出なかった。**眠っている者を見る手段が要る。**
 *
 * 60 秒ごとに task_list を舐めて pid / ppid / 状態 / 名前を出す。
 * 走査は BKL 下ではないので、**繋ぎ替えの最中に踏まないよう上限を置く**
 * (壊れたリストで無限に回るより、途中で切るほうがまし)。 */
extern struct task* task_list;   /* kernel/task.c。他所と同じ形で引く */

#define TASKS_MAX_SHOWN 24

/* **既定では黙る。**60 秒ごとの計器一式は AARCH64_VERBOSE_DIAG が
 * 立っているときだけ組み込む (2026-09-04)。呼び出し元
 * (cpu_stats_report) も同じガードの中にあるので、既定ビルドでは
 * ここから下の一連の static がまとめて消える */
#ifdef AARCH64_VERBOSE_DIAG
static const char *task_state_name(task_state_t s) {
    switch (s) {
        case TASK_RUNNING:  return "run";
        case TASK_READY:    return "rdy";
        case TASK_SLEEPING: return "slp";
        case TASK_IO_WAIT:  return "io";
        case TASK_ZOMBIE:   return "zmb";
        case TASK_DEAD:     return "dead";
        default:            return "?";
    }
}

void aarch64_pmm_scan_report(void);
void xv6log_commit_report(void);

static void tasks_report(void) {
    struct task *t = task_list;
    int n = 0;

    if (!t) return;
    aarch64_uart_puts("[tasks]");
    while (t && n < TASKS_MAX_SHOWN) {
        aarch64_uart_puts("  ");
        aarch64_uart_putdec64((uint64_t)(int64_t)t->pid);
        aarch64_uart_puts("<");
        aarch64_uart_putdec64((uint64_t)(int64_t)t->ppid);
        aarch64_uart_puts(" ");
        aarch64_uart_puts(task_state_name(t->state));
        aarch64_uart_puts(" ");
        aarch64_uart_puts(t->comm[0] ? t->comm : "?");
        t = t->next;
        n++;
    }
    if (t) aarch64_uart_puts("  ...");
    aarch64_uart_puts("\n");
}

/* **[cpu] とは別の行にする。**既存の台本が [cpu] の形を見ているので、
 * そちらは変えない。暇なコアは出さない */
static void pc_report(uint32_t n) {
    uint32_t i, k, best;
    int any = 0;

    for (i = 0; i < n; i++) {
        if (g_pc_total[i] == 0) continue;
        best = 0;
        for (k = 1; k < PC_SLOTS; k++)
            if (g_pc[i][k].hits > g_pc[i][best].hits) best = k;
        if (g_pc[i][best].hits == 0) continue;

        if (!any) { aarch64_uart_puts("[pc] 60s"); any = 1; }
        aarch64_uart_puts("  cpu");
        aarch64_uart_putdec64(i);
        aarch64_uart_puts(" pid");
        aarch64_uart_putdec64((uint64_t)(int64_t)g_cpu_pid[i]);
        aarch64_uart_puts(" ");
        aarch64_uart_puts(g_cpu_comm[i][0] ? g_cpu_comm[i] : "?");
        aarch64_uart_puts(" ");
        aarch64_uart_puthex64(g_pc[i][best].pc);
        aarch64_uart_puts(" lr=");
        aarch64_uart_puthex64(g_cpu_lr[i]);
        aarch64_uart_puts(" ");
        aarch64_uart_putdec64((uint64_t)g_pc[i][best].hits * 100ULL / g_pc_total[i]);
        aarch64_uart_puts("%");
    }
    if (any) aarch64_uart_puts("\n");

    for (i = 0; i < n; i++) {
        g_pc_total[i] = 0;
        for (k = 0; k < PC_SLOTS; k++) { g_pc[i][k].pc = 0; g_pc[i][k].hits = 0; }
    }
}
#endif /* AARCH64_VERBOSE_DIAG */

static void cpu_stats_count(uint32_t cpu, const struct aarch64_trap_frame *frame) {
    struct cpu_local* c;
    if (cpu >= AARCH64_MAX_CPUS) return;
    g_cpu_tick_all[cpu]++;
    /* **cpu_local がまだ無い時期にも tick は来る** (副コアが数に入る前)。
     * その間は「暇」として数える — 実際まだ何もしていない */
    c = get_cpu_local();
    if (!c) return;
    if (c->current_task && c->current_task != c->idle_task) {
        g_cpu_tick_busy[cpu]++;
        /* **働いている tick だけ標本を取る。**idle の wfi を数えても意味が無い */
        pc_sample(cpu, c->current_task, frame);
    }
    if (c->runq_count > g_cpu_runq_peak[cpu]) g_cpu_runq_peak[cpu] = c->runq_count;
}

/* **区間ごとの割合を出す。** 累積だと平均に均されて、ビルドの山が
 * 見えなくなる。前回からの差分で出す。
 *
 * 呼ぶのは CPU 0 だけ。**待ち行列の頂点は出したら 0 に戻す** —
 * 「この 60 秒で最大いくつ積まれたか」が見たいので */
#ifdef AARCH64_VERBOSE_DIAG
static void cpu_stats_report(void) {
    static uint64_t prev_all[AARCH64_MAX_CPUS];
    static uint64_t prev_busy[AARCH64_MAX_CPUS];
    static uint64_t prev_wait[AARCH64_MAX_CPUS][2];
    static uint64_t prev_pct;          /* 前回の CNTPCT。区間の長さに使う */
    uint64_t now, span;
    uint32_t i;
    uint32_t n = aarch64_boot_info()->cpu_count;
    if (n > AARCH64_MAX_CPUS) n = AARCH64_MAX_CPUS;

    /* **区間の長さは tick 数ではなく CNTPCT で測る。** 待ちの割合を
     * 出すには「実時間で何割」が要る。tick は 10ms 刻みで粗い */
    now  = aarch64_wait_now();
    span = now - prev_pct;
    prev_pct = now;

    aarch64_console_begin();
    aarch64_uart_puts("[cpu] 60s");
    for (i = 0; i < n; i++) {
        uint64_t all  = g_cpu_tick_all[i]  - prev_all[i];
        uint64_t busy = g_cpu_tick_busy[i] - prev_busy[i];
        uint64_t wsp  = aarch64_wait_get(i, 0) - prev_wait[i][0];   /* spinlock */
        uint64_t wsd  = aarch64_wait_get(i, 1) - prev_wait[i][1];   /* SD */
        prev_all[i]  = g_cpu_tick_all[i];
        prev_busy[i] = g_cpu_tick_busy[i];
        prev_wait[i][0] = aarch64_wait_get(i, 0);
        prev_wait[i][1] = aarch64_wait_get(i, 1);

        aarch64_uart_puts("  cpu");
        aarch64_uart_putdec64(i);
        aarch64_uart_puts(" ");
        /* tick が 0 の CPU は「上がっていない」。割り算で落ちないように */
        aarch64_uart_putdec64(all ? (busy * 100ULL) / all : 0ULL);
        /* **待ちの内訳。** busy のうち何割が待ちかがここで分かれる。
         * lk = spinlock を取るまで / sd = SD の応答待ち */
        aarch64_uart_puts("%(lk");
        aarch64_uart_putdec64(span ? (wsp * 100ULL) / span : 0ULL);
        aarch64_uart_puts(" sd");
        aarch64_uart_putdec64(span ? (wsd * 100ULL) / span : 0ULL);
        aarch64_uart_puts(" rq");
        aarch64_uart_putdec64(g_cpu_runq_peak[i]);
        aarch64_uart_puts(")");
        g_cpu_runq_peak[i] = 0;
    }
    aarch64_uart_puts("\n");
    /* **誰がどこを回っているか。**[cpu] が「焼けている」と言った直後に、
     * その中身を名指しする */
    pc_report(n);
    /* **眠っている者も含めて全部出す。**wait4 で待たれている子が
     * どの状態で止まっているかは、ここにしか出ない */
    tasks_report();
    aarch64_console_end();

    /* **SD への入出力を回数で出す (P-1)。**[cpu] の行と同じ 60 秒の区間で
     * 並べて読めるように、続けて出す。**量では説明がつかないと分かって
     * いる**ので、見たいのは回数と 1 回あたりの待ち (日報2026-08-29 §14) */
    aarch64_emmc2_io_report();

    /* **物理ページの確保が何ページ走っているか (P-10 の計器)。**
     * next-fit が効いていれば 1 回あたり数ページ、効いていなければ
     * 数十万ページになる。原因を推し量らずに済ませるために出す */
    aarch64_pmm_scan_report();

    /* **ログのコミットがどう束ねられているか。**
     * SD の書き込みが 1 回 15ms かかっていたので、区間長が伸びているかを
     * 並べて読めるようにする (2026-08-30) */
    xv6log_commit_report();
}
#endif /* AARCH64_VERBOSE_DIAG */

void aarch64_timer_on_tick(struct aarch64_trap_frame* frame) {
    /* **周期のポーリングは CPU 0 だけが行う (D-5)。**
     *
     * タイマは PPI なので 4 コア全部に上がる (gic.c:100)。ここを
     * 全コアで回すと、どちらもグローバルに 1 組しか無い状態を
     * 書くので壊れる — kbd は xHCI のイベントリングを取り合い
     * (usb.c:2611)、sound は g_clean_idx を同時に進める。
     *
     * **ロックで包むのではなく、叩く者を 1 人に戻す。** どちらも
     * 「定期的に 1 回」が意味で、四重に叩く意味が無い。ロックだと
     * kbd_tick の USB 転送 (ms 単位) を**割り込み文脈のまま 3 コアが
     * 待つ**ことになる。sched.c:133 の task_poll_sleep_wakeups と同じ形。
     *
     * **周期も直る。** 4 コアで回していた間は「10ms に 1 回」の
     * つもりで 4 倍叩いていた。
     *
     * **これで消えるのは tick 同士だけ。** tick と syscall (別のコアで
     * 走るユーザプロセス) の競合は残るので、そちらは kbd.c / sound.c
     * 側でロックを取る */
    uint32_t me = timer_cpu_index();

    /* **数えるのは全 CPU。** 出すのは CPU 0 だけ (下) */
    cpu_stats_count(me, frame);

    if (me == 0) {
        /* **USB キーボードを拾う。**割り込みを繋いでいないので、ここが唯一の
         * 定期的な機会。文字が来ていればコンソールのリングへ流し、寝ている
         * シェルを起こす (kernel/aarch64/kbd.c)。
         * **キーボードが無い機械では即座に戻る** */
        aarch64_kbd_tick();

        /* 音の後片付け (D-3)。**鳴っていなければ即座に戻る** */
        sound_tick();

        /* **数えるのも CPU 0 だけ (D-5)。**
         *
         * これは「割り込みが入った回数」で、time ではなく**間隔**の
         * 物差しに使われている (virtio_blk_mmio.c:43 の
         * 「10ms x 300 = 3 秒」)。4 コアで数えると 4 倍の速さで進み、
         * **3 秒のつもりが 0.75 秒で時間切れになる。**
         * 非 atomic な ++ を 4 コアで叩いて数を落とす問題も同時に消える。
         *
         * 時刻 (arch_time_now_ms) は CNTPCT_EL0 から出しているので
         * ここには依存しない (runtime.c:44) */
        g_ticks++;

        /* 60 秒ごとに 1 行。100Hz なので 6000 tick。
         * **既定では黙る (AARCH64_VERBOSE_DIAG)。** ash で作業中に 1 分おき
         * 割り込むのは実使用では邪魔 (2026-09-04)。性能調査のときだけ足す */
#ifdef AARCH64_VERBOSE_DIAG
        if ((g_ticks % 6000ULL) == 0ULL) cpu_stats_report();
#endif
    }

    aarch64_task_on_tick();   /* 切り替えの印を立てる (実際の切り替えは IRQ の出口) */
    write_tval(g_interval);
    write_ctl(CNTP_CTL_ENABLE);
}
