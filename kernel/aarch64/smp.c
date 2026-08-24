/*
 * aarch64 の副コア起動 (SMP の P-4)。
 *
 * 設計と段階は AArch64-SMP-Design.md。riscv64 の kernel/riscv64/smp.c に
 * 対応するが、**まだ共有スケジューラには載せない** — P-4 の目的は
 * 「起きて、MMU を入れて、上位 VA で 1 行出す」ところまで。
 *
 * 起こし方は 2 つあり、**DTB の enable-method で選ぶ。機械名で分岐しない。**
 *
 *   psci        QEMU virt。HVC で CPU_ON を呼ぶ
 *   spin-table  Raspberry Pi 4 (実機 / QEMU raspi4b)。P-8 で足す
 *
 * ---- PSCI が使えない構成がある ------------------------------------------
 *
 * /psci の method は "hvc"。**`-machine virt,virtualization=on` では
 * 自分で EL2 から EL1 へ降りている** (start.S の 6:) ため、EL1 からの HVC は
 * VBAR_EL2 を見に行く。そこに表を置いていないのでハングする。
 * **既定の `-machine virt` (EL1 起動) でだけ使うこと。**
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/task.h"
#include "aarch64/vm.h"
#include "smp.h"
#include "task.h"
#include "spinlock.h"

extern char __kernel_end[];

/* entry.S */
uint64_t aarch64_secondary_entry_pa(void);
/* boot.c / vectors.S */
void aarch64_vectors_init(void);
void aarch64_uart_puts(const char* s);
void aarch64_uart_puthex64(uint64_t v);
void aarch64_console_begin(void);
void aarch64_console_end(void);
/* gic.c / timer.c */
void aarch64_gic_init_cpu(void);
void aarch64_gic_send_sgi(uint32_t cpu_mask, unsigned intid);
void aarch64_timer_init(void);

/* resched IPI に使う SGI の番号。**0-15 のどれでもよい**が、
 * 0 を使うのは riscv64 が IPI を 1 種類しか持たないのに合わせたため */
#define AARCH64_SGI_RESCHED  0

/* PSCI 1.0 の CPU_ON (64bit 呼び出し規約)。**QEMU virt の /psci が
 * cpu_on = 0xc4000003 と申告しているのを実測した値と一致** */
#define PSCI_CPU_ON_64      0xC4000003ULL
#define PSCI_SUCCESS        0

/* 起きたコアの数 (CPU 0 を含まない)。**副コアが上位 VA へ移り切ってから
 * 増やす** ので、これが増えていれば MMU の切り替えまで通ったことになる */
static volatile uint32_t g_secondary_online;

/* ---- 共有層から見た CPU の一覧 (SMP の P-5) -------------------------------
 *
 * **これを実装した瞬間にスケジューラがタスクを分散し始める。**
 * kernel/task.c:316 の choose_spawn_cpu_locked が
 * `smp_get_started_cpu_count() <= 1` で早期に戻る作りなので、ここが 2 を
 * 返すかどうかが「SMP を使うか」の唯一のスイッチになっている。
 *
 * **だから smp_send_resched_ipi より先に 2 を返してはいけない。**
 * 起こす手段が無いまま CPU 1 にタスクを置くと、その CPU は wfi で
 * 寝たまま誰にも起こされず、タスクが永久に走らない。
 *
 * **数えるのは共有スケジューラに載り切ったコアだけ** (idle を作り、
 * cpu_local を設置し、割り込みを開けた後)。MMU が入っただけの段階で
 * 数に入れると、まだ受け皿の無い CPU にタスクが飛ぶ。 */
static struct smp_cpu_info g_cpus[AARCH64_MAX_CPUS];
static volatile uint32_t g_started_cpus = 1;   /* CPU 0 は最初から動いている */

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index >= AARCH64_MAX_CPUS) return 0;
    return &g_cpus[cpu_index];
}

uint32_t smp_get_started_cpu_count(void) { return g_started_cpus; }

/* 他の CPU を起こす。**自分に送る必要は無い** — 呼ぶ側 (kernel/sched.c の
 * task_request_resched_cpu) が自 CPU を除いてから呼ぶ。
 *
 * 宛先は GICD の CPU インタフェース番号。**MPIDR の Aff0 と同じとは
 * 限らない**が、QEMU virt も Pi 4 も Aff0 が 0..3 でそのまま対応する。
 * 対応しない機械が出たら GICD_ITARGETSR を読んで自分の番号を知る手が要る */
void smp_send_resched_ipi(uint32_t cpu_id) {
    if (cpu_id >= AARCH64_MAX_CPUS) return;
    if (!g_cpus[cpu_id].started) return;
    aarch64_gic_send_sgi(1U << (g_cpus[cpu_id].lapic_id & 7U), AARCH64_SGI_RESCHED);
}

/* IRQ ハンドラ (boot.c) が SGI を受けたときに呼ぶ。**受信を数えるだけ。**
 * 実際の切り替えは task_idle_loop / IRQ の出口が resched の印を見て行う */
static volatile uint32_t g_ipi_seen[AARCH64_MAX_CPUS];

void aarch64_smp_on_ipi(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    g_ipi_seen[(uint32_t)(mpidr & 0xffULL) % AARCH64_MAX_CPUS]++;
}

/* **IPI が本当に届くかを 1 回だけ確かめる。**
 *
 * x86 の smp_send_resched_ipi_selftest (kernel/init.c:299) と同じ位置づけ。
 * ここが通らないまま先へ進むと、「タスクを置いたのに走らない」という
 * 一段離れた形で出てきて原因が遠くなる。 */
static int aarch64_smp_ipi_selftest(uint32_t cpu_id) {
    uint32_t before = g_ipi_seen[cpu_id % AARCH64_MAX_CPUS];
    uint64_t spin = 0;

    smp_send_resched_ipi(cpu_id);
    while (g_ipi_seen[cpu_id % AARCH64_MAX_CPUS] == before && spin < 100000000ULL) {
        __asm__ volatile("yield" ::: "memory");
        spin++;
    }
    return g_ipi_seen[cpu_id % AARCH64_MAX_CPUS] != before;
}

/* ---- PSCI 呼び出し --------------------------------------------------------
 *
 * SMCCC は x0-x3 が引数で、**戻り値も x0-x3 に返る** (呼び出し側から見れば
 * 4 本とも壊れる)。"+r" で 4 本とも書き換わることを伝える */
static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("hvc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "memory");
    return (int64_t)x0;
}

/* ---- spin-table で起こす (SMP の P-8) ------------------------------------
 *
 * Raspberry Pi 4 には PSCI が無い (include/aarch64/bcm_periph.h:35)。
 * 副コアはファームウェア (armstub8 / QEMU の hw/arm/raspi.c) が置いた
 * 短いループの中で **release address をポーリングしながら wfe で寝ている**。
 *
 * QEMU のソースで確認した相手側のループ:
 *
 *     mov x5, 0xd8 / mrs x6, mpidr_el1 / and x6, x6, #3
 *     spin: wfe / ldr x4, [x5, x6, lsl #3] / cbz x4, spin / br x4
 *
 * ここから手順が 3 つ決まる。
 *
 *   1. **書くのは物理アドレス。** 相手は MMU off で飛ぶ
 *   2. **`dc civac` が要る。** 相手はキャッシュ off で読むので、
 *      こちらの書き込みが D-cache に残っていると見えない。
 *      **2026-08-23 に exec で踏んだ I-cache の話の裏返し**
 *   3. **`sev` が要る。** wfe で寝ているので、書くだけでは起きない
 *
 * release address は低位 RAM (0xd8-0xf0) に在る。**カーネルより手前**なので
 * 上位 VA の直線マップ経由で触る。 */
static int aarch64_smp_wake_spin_table(uint64_t release_pa, uint64_t entry_pa) {
    volatile uint64_t* slot;

    if (!release_pa) return -1;

    slot = (volatile uint64_t*)(uintptr_t)aarch64_phys_to_virt(release_pa);
    *slot = entry_pa;

    /* PoC まで掃き出す。相手はキャッシュ off で読む */
    __asm__ volatile("dc civac, %0" :: "r"(slot) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("sev");
    return 0;
}

/* ---- 副コア: 上位 VA に移った後 ------------------------------------------
 *
 * ここからは CPU 0 と同じ世界。**VBAR_EL1 は CPU ごとのレジスタ**なので、
 * 自分で張らないと例外が起きても何も出ずに沈黙する。 */
static void aarch64_secondary_high(void) {
    uint64_t mpidr;
    uint32_t cpu_index;
    struct task* idle;

    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    cpu_index = (uint32_t)(mpidr & 0xffULL);

    /* **VBAR_EL1 は CPU ごと。** 張らないと例外が起きても沈黙する */
    aarch64_vectors_init();

    /* **CPACR_EL1.FPEN も CPU ごと (P-5 で踏んだ)。**
     *
     * start.S がこれを開けているのは `4:` の CPU 0 の経路だけ。副コアは
     * そこを通らないので、**EL0 が FP/NEON 命令を使った瞬間にトラップする。**
     *
     * 実測: `-smp 4` で ash を動かすと echo / pwd / 変数代入までは通り、
     * `uname -m` で落ちた。
     *
     *   ESR 0x1fe00000  EC=0x07 (FP アクセスが CPACR_EL1.FPEN でトラップ)
     *   FAR 0           ELR はカーネルの中
     *
     * **musl は memcpy などで NEON を使う**ので、タスクが副コアに移った
     * 最初の機会に出る。**1 コアでは絶対に踏めない。**
     * FPEN = 0b11: EL0 / EL1 のどちらからも trap しない (start.S と同じ値) */
    {
        uint64_t cpacr;
        __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
        cpacr |= (3ULL << 20);
        __asm__ volatile("msr cpacr_el1, %0" :: "r"(cpacr));
        __asm__ volatile("isb");
    }

    /* ---- 共有スケジューラに載る (P-5) ------------------------------------
     *
     * **順序は kernel/riscv64/smp.c:91 のとおり。** あちらのコメントに
     * ある落とし穴をそのまま避ける:
     *
     *   enter -> create_idle -> bind -> exit -> install
     *
     * **install をロックの中でやってはいけない。** この時点では自 CPU の
     * cpu_local が未設置なので kernel_lock_enter/exit は深さ管理ではなく
     * 生のスピンロックとして振る舞う (kernel/aarch64/runtime.c)。
     * ロック中に install すると、exit 側が depth==0 を見て**解放せずに
     * 戻り、全 CPU が止まる。** */
    kernel_lock_enter();
    idle = task_create_idle(cpu_index);
    if (idle) {
        task_bind_cpu_local(cpu_index, idle, idle, idle->kstack_top);
    }
    kernel_lock_exit();

    if (!idle) {
        aarch64_console_begin();
        aarch64_uart_puts("[smp] cpu");
        aarch64_uart_puthex64(cpu_index);
        aarch64_uart_puts(" idle タスクを作れない\n");
        aarch64_console_end();
        for (;;) __asm__ volatile("wfe");
    }

    task_install_cpu_local(cpu_index);
    aarch64_trap_set_kernel_stack(idle->kstack_top);

    /* **CPU Interface とタイマはコアごとに開ける。** どちらも
     * バンクされたレジスタで、CPU 0 が開けても効かない */
    aarch64_gic_init_cpu();
    aarch64_timer_init();

    /* **数に入れるのは受け皿が全部そろった後。** ここを先にすると、
     * まだ idle も cpu_local も無い CPU にタスクが飛ぶ */
    g_cpus[cpu_index].started = 1;
    __atomic_add_fetch(&g_started_cpus, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&g_secondary_online, 1, __ATOMIC_SEQ_CST);

    aarch64_console_begin();
    aarch64_uart_puts("[smp] cpu");
    aarch64_uart_puthex64(cpu_index);
    aarch64_uart_puts(" online (共有スケジューラに参加)\n");
    aarch64_console_end();

    __asm__ volatile("msr daifclr, #2");   /* 割り込みを開ける */

    task_idle_loop(0);
    for (;;) __asm__ volatile("wfe");
}

/* ---- 副コア: MMU off・物理 PC で呼ばれる ---------------------------------
 *
 * start.S の aarch64_secondary_start から blr で来る。
 *
 * **ここで上位 VA のシンボルを触ってよい理由:** この関数は上位 VA で
 * リンクされているが PC は物理なので、adrp (PC 相対) の結果も物理になる。
 * _start が aarch64_early_main を呼ぶのと同じ約束。
 * **絶対アドレスを literal で持つような書き方をしないこと。**
 *
 * ページテーブルは CPU 0 が組んだものをそのまま使う (共有)。 */
void aarch64_secondary_main(uint64_t cpu_index) {
    uint64_t kend_pa, new_sp, cont;

    /* PC が物理なので、シンボルのアドレスも物理で返る */
    kend_pa = (uint64_t)(uintptr_t)__kernel_end;

    aarch64_mmu_enable();

    /* **スタックを上位 VA に取り直してから飛ぶ。** いまのスタックには
     * 物理の戻り先が積まれていて、恒等マッピングを外すと破綻する。
     * 割り当ての式は start.S と同じ (base + (cpu+1) * 64KB) */
    new_sp = aarch64_phys_to_virt(kend_pa + (cpu_index + 1ULL) * 65536ULL);
    cont   = aarch64_phys_to_virt((uint64_t)(uintptr_t)aarch64_secondary_high);

    aarch64_vm_enter_high(new_sp, cont);   /* 戻ってこない */
}

/* ---- CPU 0 から呼ぶ -------------------------------------------------------
 *
 * **MMU を入れて上位 VA へ移った後に呼ぶこと。** 渡す入口は物理アドレス
 * (副コアは MMU off で来るため)。 */
void aarch64_smp_start_secondaries(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t entry = aarch64_secondary_entry_pa();
    uint32_t n = b->cpu_count;
    uint32_t want = 0;

    if (n > AARCH64_MAX_CPUS) n = AARCH64_MAX_CPUS;

    aarch64_console_begin();
    aarch64_uart_puts("--- SMP: 副コアを起こす ---\n  entry     : ");
    aarch64_uart_puthex64(entry);
    aarch64_uart_puts("\n");
    aarch64_console_end();

    /* **一覧は先に埋める。** smp_get_cpu_info は共有層 (task.c の
     * task_choose_rebalance_cpu_locked) が起動直後から引く */
    for (uint32_t i = 0; i < n; i++) {
        g_cpus[i].cpu_index   = i;
        g_cpus[i].processor_id = (uint32_t)b->cpu_mpidr[i];
        g_cpus[i].lapic_id     = (uint32_t)b->cpu_mpidr[i];
        g_cpus[i].is_bsp       = (i == 0);
        g_cpus[i].started      = (i == 0);
    }

    if (n < 2) {
        aarch64_uart_puts("  cpus      : 1 本しか無い。何もしない\n");
        aarch64_uart_puts("aarch64-smp-skip\n");
        return;
    }

    /* **cpu_local の枠を開ける。** get_cpu_local_by_id は
     * `cpu_id >= g_cpu_count` を弾く (kernel/task.c:150) ので、
     * これを先に呼ばないと副コアの task_bind_cpu_local が黙って失敗する。
     *
     * **これ自体はタスクの分散を始めない。** 分散の引き金は
     * smp_get_started_cpu_count() のほう (上の注記) */
    task_set_cpu_count(n);

    /* **見つかった副コアを全部起こす (P-6)。** P-4/P-5 は 1 本に絞って
     * いたが、1 本で通ったので広げた */
    for (uint32_t i = 1; i < n; i++) {
        int64_t ret;

        if (b->cpu_enable_method[i] == AARCH64_CPU_ENABLE_SPIN_TABLE) {
            want++;
            ret = aarch64_smp_wake_spin_table(b->cpu_release_addr[i], entry);
            aarch64_console_begin();
            aarch64_uart_puts("  spin-table: mpidr ");
            aarch64_uart_puthex64(b->cpu_mpidr[i]);
            aarch64_uart_puts("  release ");
            aarch64_uart_puthex64(b->cpu_release_addr[i]);
            aarch64_uart_puts(ret == 0 ? "  ok\n" : "  BAD (release 番地が無い)\n");
            aarch64_console_end();
            if (ret != 0) want--;
            continue;
        }

        if (b->cpu_enable_method[i] != AARCH64_CPU_ENABLE_PSCI) {
            /* **黙って飛ばさず、理由を出す** */
            aarch64_console_begin();
            aarch64_uart_puts("  cpu       : ");
            aarch64_uart_puthex64(b->cpu_mpidr[i]);
            aarch64_uart_puts("  起こし方が分からないので飛ばす\n");
            aarch64_console_end();
            continue;
        }

        want++;
        ret = psci_call(PSCI_CPU_ON_64, b->cpu_mpidr[i], entry, i);
        /* **CPU_ON が返った時点で相手はもう走っている。** 報告を囲まないと
         * 自分の行に相手の「online」が混ざる */
        aarch64_console_begin();
        aarch64_uart_puts("  CPU_ON    : mpidr ");
        aarch64_uart_puthex64(b->cpu_mpidr[i]);
        aarch64_uart_puts("  ret ");
        aarch64_uart_puthex64((uint64_t)ret);
        aarch64_uart_puts(ret == PSCI_SUCCESS ? "  ok\n" : "  BAD\n");
        aarch64_console_end();
        if (ret != PSCI_SUCCESS) want--;
    }

    if (want == 0) {
        aarch64_uart_puts("aarch64-smp-skip (起こせる副コアが無い)\n");
        return;
    }

    /* **待ちは有限にする。** 上がらないときに黙って止まると、
     * どこまで進んだのか分からなくなる */
    {
        uint64_t spin = 0;
        while (g_secondary_online < want && spin < 200000000ULL) {
            __asm__ volatile("yield" ::: "memory");
            spin++;
        }
    }

    /* **1 行が複数回の puts でできているものは、明示的に囲む。**
     * ロックの単位は begin/end の対であって puts 1 回ではない。
     * 囲まずに P-4 を回したら、ちょうどこの行に副コアの
     * 「[smp] cpuN online」が割り込んだ (実測) */
    aarch64_console_begin();
    aarch64_uart_puts("  online    : ");
    aarch64_uart_puthex64(g_secondary_online);
    aarch64_uart_puts(" / ");
    aarch64_uart_puthex64(want);
    aarch64_uart_puts(g_secondary_online >= want ? "\n" : "\naarch64-smp-BAD\n");
    aarch64_console_end();

    if (g_secondary_online < want) return;

    /* **IPI が届くことを、タスクを載せる前に確かめる** */
    for (uint32_t i = 1; i < n; i++) {
        if (!g_cpus[i].started) continue;
        aarch64_console_begin();
        aarch64_uart_puts("  ipi cpu   : ");
        aarch64_uart_puthex64(i);
        aarch64_uart_puts(aarch64_smp_ipi_selftest(i) ? "  ok (SGI が届いた)\n"
                                                      : "  BAD (SGI が届かない)\n");
        aarch64_console_end();
    }
    aarch64_uart_puts("aarch64-smp-ok\n");
}
