/* ---- 誰が、どこを回っているか ([pc]) --------------------------------------
 *
 * **aarch64 に在って x86 に無かった計器** (kernel/aarch64/timer.c:118-283)。
 * 日報2026-09-09 §12 で `wait4` の待ち方を共通化したとき、
 * **「CPU を焼かなくなった」を x86 で測れなかった。**ホストの CPU 時間は
 * idle の起き直しに埋もれ、ゲスト側には計器が無かった。
 *
 * 中身は aarch64 と同じ、既存の 10ms タイマ tick に相乗りした標本化
 * プロファイラ。**追加の割り込みも入力の口も要らない。**割り込んだ地点の
 * PC はトラップフレームの `rip` にそのまま入っている。
 *
 * 1 標本だけだと「たまたまそこ」を掴むので、**16 バイト単位に丸めて
 * 4 枠の多数決**を取る。区間で最も多かった枠と、その割合を出す。
 *
 * PC の上位が 0xffffffff80... ならカーネル (scripts/kernel.ld:8 で固定、
 * -fno-PIE)。`nm kernel.elf` で直接引ける。
 *
 * **aarch64 の `lr=` に当たるものは出していない。**あちらは x30 を
 * フレームから只で採れるが、x86 に LR は無く、-O2 の clang は rbp を
 * フレームポインタに使っていない (Makefile:29 に -fno-omit-frame-pointer が
 * 無い)。足すには全関数の生成コードが動くので、**要ると分かってから
 * 決める。**IRQ を開けたまま回るループ (sys_wait4 がそれ) は PC だけで
 * 関数を指すので、いま欲しいものはこれで足りる。 */

#ifdef X86_VERBOSE_DIAG

#include "task.h"
#include "smp.h"
#include <stdint.h>
#include <stddef.h>

void puts(const char* s);

void x86_pc_sample(uint64_t rip);
void x86_pc_report(void);
void x86_pc_mark_idle_loop(void);

#define PC_SLOTS 4
/* **64 では粗すぎた** —— aarch64 が 2026-08-29 に読み違えた粒度
 * (kernel/aarch64/timer.c:137)。関数が混ざらない粒度に合わせる */
#define PC_GRAIN 16ULL
/* ORTHOX_MAX_CPUS は 64 だが、そこまで確保すると BSS だけで 5KB 以上になる。
 * 実際に起こしている数に合わせる (足りなければここを上げる) */
#define PC_MAX_CPUS 8

struct pc_slot { uint64_t pc; uint32_t hits; };
static struct pc_slot     g_pc[PC_MAX_CPUS][PC_SLOTS];
static volatile uint32_t  g_pc_total[PC_MAX_CPUS];
static volatile int       g_pc_pid[PC_MAX_CPUS];
static char               g_pc_comm[PC_MAX_CPUS][16];

/* ---- 起動タスクも暇 (x86 だけの形) --------------------------------------
 *
 * **x86 の起動タスクは idle_task にならないまま idle ループへ落ちる**
 * (kernel/x86_64/init.c:332 の task_idle_loop(1))。`task_is_idle_task` は
 * `pid == 0` で判定するが (kernel/task.c:364)、起動タスクは pid1 なので
 * どちらの条件にも当たらない。
 *
 * **印を付けないと、hlt で寝ている時間まで「働いている」と数える。**
 * 実際に最初の計測がそうなった —— cpu0 が `task_idle_loop+0x20` で 99%。
 * 桶 (16 バイト) の中身は `sti` / `hlt` / hlt の直後で、**寝ていることの
 * 証拠そのもの**だった。
 *
 * aarch64 と riscv64 にこの形は無い。`task_idle_loop` を呼ぶのは
 * idle_task_entry (kernel/task.c:121) と AP の入口だけで、
 * **起動タスクがここへ来るのは x86 だけ。**
 *
 * 呼び手は 1 箇所・1 度きり (戻らない関数の直前) なので、枠は 1 つ、
 * 競合も無い。 */
static struct task* volatile g_boot_idle_task;

void x86_pc_mark_idle_loop(void) {
    g_boot_idle_task = get_current_task();
}

/* 自分のコアの枠だけを触るのでロックは要らない。
 * **呼ぶのは kernel_lock_enter() の前** (kernel/x86_64/idt.c)。
 * BKL の後に置くと、埋まっている tick だけ待たされて標本の分布が歪む */
void x86_pc_sample(uint64_t rip) {
    struct cpu_local* c = get_cpu_local();
    struct task* t;
    uint32_t cpu, i, worst = 0;
    int j;

    if (!c) return;
    cpu = c->cpu_id;
    if (cpu >= PC_MAX_CPUS) return;

    t = c->current_task;
    /* **働いている tick だけ標本を取る。**idle の hlt
     * (include/x86_64/task.h:87) を数えても意味が無い。
     * 3 つ目は起動タスク —— 上の g_boot_idle_task を参照 */
    if (!t || t == c->idle_task || t == g_boot_idle_task) return;

    rip &= ~(PC_GRAIN - 1ULL);

    /* **誰かは毎回書き換える。**枠に当たったときだけ書いていると、
     * 最初にその番地を踏んだタスクの名前が残り続ける (aarch64 の
     * timer.c:163 で踏んだもの) */
    g_pc_pid[cpu] = t->pid;
    for (j = 0; j < 15; j++) {
        char ch = t->comm[j];
        g_pc_comm[cpu][j] = ch;
        if (!ch) break;
    }
    g_pc_comm[cpu][15] = '\0';

    g_pc_total[cpu]++;
    for (i = 0; i < PC_SLOTS; i++) {
        if (g_pc[cpu][i].hits && g_pc[cpu][i].pc == rip) { g_pc[cpu][i].hits++; return; }
        if (g_pc[cpu][i].hits < g_pc[cpu][worst].hits) worst = i;
    }
    /* 空きか、いちばん少ない枠を置き換える */
    g_pc[cpu][worst].pc = rip;
    g_pc[cpu][worst].hits = 1;
}

/* ---- 出力 ---------------------------------------------------------------
 *
 * **1 行を組み立ててから puts を 1 回だけ叩く。**x86 の puts は 1 回ごとに
 * シリアルのロックを取る (kernel/x86_64/init.c:99) ので、細切れに呼ぶと
 * 他の CPU の行が割り込んで混ざる。aarch64 は console_begin/end で
 * 囲っているが、こちらは組み立ててしまうほうが簡単で、新しいロックも
 * 要らない。 */

static void append_str(char* buf, int* pos, int max, const char* s) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}

static void append_dec(char* buf, int* pos, int max, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos < max - 1) buf[(*pos)++] = tmp[--n];
}

static void append_hex64(char* buf, int* pos, int max, uint64_t v) {
    const char* hex = "0123456789abcdef";
    int i;
    for (i = 60; i >= 0 && *pos < max - 1; i -= 4) buf[(*pos)++] = hex[(v >> i) & 0xF];
}

/* **暇なコアは出さない。**区間ごとの割合を出したいので、出したら 0 に戻す。
 * 累積だと平均に均されて、ビルドの山が見えなくなる */
void x86_pc_report(void) {
    /* CPU 0 しか呼ばず、割り込みゲートなので同じ CPU で入れ子にもならない。
     * 割り込み文脈のスタックに 640 バイト積まないための static */
    static char line[640];
    int pos = 0;
    uint32_t n = smp_get_started_cpu_count();
    uint32_t i, k, best;
    int any = 0;

    if (n == 0) n = 1;
    if (n > PC_MAX_CPUS) n = PC_MAX_CPUS;

    for (i = 0; i < n; i++) {
        if (g_pc_total[i] == 0) continue;
        best = 0;
        for (k = 1; k < PC_SLOTS; k++)
            if (g_pc[i][k].hits > g_pc[i][best].hits) best = k;
        if (g_pc[i][best].hits == 0) continue;

        if (!any) { append_str(line, &pos, sizeof(line), "[pc] 60s"); any = 1; }
        append_str(line, &pos, sizeof(line), "  cpu");
        append_dec(line, &pos, sizeof(line), i);
        append_str(line, &pos, sizeof(line), " pid");
        append_dec(line, &pos, sizeof(line), (uint64_t)(int64_t)g_pc_pid[i]);
        append_str(line, &pos, sizeof(line), " ");
        append_str(line, &pos, sizeof(line), g_pc_comm[i][0] ? g_pc_comm[i] : "?");
        append_str(line, &pos, sizeof(line), " 0x");
        append_hex64(line, &pos, sizeof(line), g_pc[i][best].pc);
        append_str(line, &pos, sizeof(line), " ");
        append_dec(line, &pos, sizeof(line), (uint64_t)g_pc[i][best].hits * 100ULL / g_pc_total[i]);
        append_str(line, &pos, sizeof(line), "%");
    }
    if (any) {
        append_str(line, &pos, sizeof(line), "\r\n");
        line[pos] = '\0';
        puts(line);
    }

    /* **起こした数が減っていても取りこぼさないよう全部戻す** */
    for (i = 0; i < PC_MAX_CPUS; i++) {
        g_pc_total[i] = 0;
        for (k = 0; k < PC_SLOTS; k++) { g_pc[i][k].pc = 0; g_pc[i][k].hits = 0; }
    }
}

#endif /* X86_VERBOSE_DIAG */
