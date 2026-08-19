/*
 * 共有層の下敷き (M3c-2a)。
 *
 * kernel/riscv64/runtime.c と同じ位置づけ。共有層 (kernel 直下の C) が
 * 「どのアーキでもあるもの」として呼ぶ関数を、aarch64 の実物に繋ぐ。
 *
 * **kernel/spinlock.c は使わない。** riscv64 と同じで、ロックと割り込みの
 * 開け閉めはアーキ固有なのでこちらに置く。共有版は x86 の割り込み操作を
 * 直接書いている。
 *
 * まだ載せていないもの (M3c-2b で埋める):
 *   kernel_yield    schedule() を呼ぶ形にする。いまは M3c-1 の器を回す
 *   kernel_lock_*   CPU ごとの深さにする。いまは単一 CPU 前提の 1 本
 *   コンソール入力   riscv64_console_* 相当。PL011 の受信割り込みが要る
 */
#include <stdint.h>
#include <stddef.h>
#include "aarch64/boot.h"
#include "aarch64/task.h"
#include "aarch64/vm.h"
#include "spinlock.h"

/* 物理 → カーネル VA の差。**pmm_init が実際の値を入れる。**
 * ここでは定義だけ (riscv64 も runtime.c に置いている) */
uint64_t g_hhdm_offset;

uint64_t aarch64_timer_freq(void);
uint64_t aarch64_timer_ticks(void);
int aarch64_virtio_blk_present(void);
uint64_t aarch64_virtio_blk_capacity(void);
int aarch64_virtio_blk_storage_read(void* ctx, uint64_t lba, void* buf, size_t count);
int aarch64_virtio_blk_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count);
/* Raspberry Pi 4 の SD カード (EMMC2)。**QEMU virt には無い**ので
 * aarch64_emmc2_init が -1 を返す */
int aarch64_emmc2_init(void);
uint64_t aarch64_emmc2_blocks(void);
int aarch64_emmc2_storage_read(void* ctx, uint64_t lba, void* buf, size_t count);
int aarch64_emmc2_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count);

/* ---- 時刻 ----------------------------------------------------------------
 *
 * **tick 数ではなくカウンタから出す。** tick は 10ms 刻みなので、
 * それを ms に使うと分解能が 10ms のまま固定される。CNTPCT_EL0 は
 * 単調増加のカウンタなので、周波数で割れば素直に ms になる。 */
static inline uint64_t read_cntpct(void) {
    uint64_t v;
    __asm__ volatile("isb");
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t arch_time_now_ms(void) {
    uint64_t freq = aarch64_timer_freq();
    /* 0 のまま割ると落ちる。timer.c と同じ既定値に退く */
    if (freq == 0) freq = 62500000ULL;
    return read_cntpct() / (freq / 1000ULL);
}

uint64_t arch_time_cpu_ms(uint32_t cpu_id) {
    (void)cpu_id;
    return arch_time_now_ms();
}

/* ---- 割り込みの開け閉め --------------------------------------------------
 *
 * riscv64 の sstatus.SIE に当たるのが DAIF の I (bit 7)。
 * **戻すときは元の値をそのまま書かず、I だけを復元する。** 他のビット
 * (F/A/D) を巻き戻すと、呼び出し元が意図して変えたものを消す */
#define DAIF_I  (1ULL << 7)

uint64_t irq_save_disable(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #2");
    return daif;
}

void irq_restore(uint64_t flags) {
    if (flags & DAIF_I) {
        __asm__ volatile("msr daifset, #2");
    } else {
        __asm__ volatile("msr daifclr, #2");
    }
}

/* ---- スピンロック --------------------------------------------------------
 *
 * **CPU 1 本でも要る。** 割り込みハンドラと通常の実行が同じデータを
 * 触るので、irqsave の版が本体。単純な exchange のループ */
void spinlock_init(spinlock_t* lock) {
    if (!lock) return;
    lock->locked = 0;
}

void spin_lock(spinlock_t* lock) {
    if (!lock) return;
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        /* SMP に進んだら wfe / sevl で待つ形にする。1 本のうちは意味が無い */
        __asm__ volatile("yield");
    }
}

void spin_unlock(spinlock_t* lock) {
    if (!lock) return;
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

uint64_t spin_lock_irqsave(spinlock_t* lock) {
    uint64_t flags = irq_save_disable();
    spin_lock(lock);
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* lock, uint64_t flags) {
    spin_unlock(lock);
    irq_restore(flags);
}

/* ---- BKL -----------------------------------------------------------------
 *
 * **いまは CPU 1 本前提の深さ 1 本。** riscv64 は cpu_local に持たせて
 * いるが、そちらは共有タスク層が居て初めて成り立つ。M3c-2b で
 * cpu_local に移すこと。 */
static spinlock_t g_kernel_lock;
static uint32_t g_kernel_lock_depth;

void kernel_lock_enter(void) {
    uint64_t flags = irq_save_disable();
    if (g_kernel_lock_depth == 0) spin_lock(&g_kernel_lock);
    g_kernel_lock_depth++;
    irq_restore(flags);
}

void kernel_lock_exit(void) {
    uint64_t flags = irq_save_disable();
    if (g_kernel_lock_depth != 0) {
        g_kernel_lock_depth--;
        if (g_kernel_lock_depth == 0) spin_unlock(&g_kernel_lock);
    }
    irq_restore(flags);
}

int kernel_lock_held(void) {
    return g_kernel_lock_depth != 0;
}

/* ---- コンソール ----------------------------------------------------------
 *
 * 共有層は puts / puthex で出す。**aarch64 の UART に繋ぐだけ。**
 * 1 行が並行に割れないよう、出力の単位を囲む (日報2026-08-09 追6-5) */
void puts(const char* s) {
    aarch64_uart_puts(s);
}

/* **"0x" は付けない。**呼ぶ側が `puts("0x")` を書く前提で、x86_64 の
 * `kernel/init.c` の puthex がそうなっている。ここだけ prefix を付けていたので
 * 共有層 (usb.c など) のログが `0x0x0000000000000009` になっていた
 * (2026-08-19)。**"0x" 付きが欲しいときは aarch64_uart_puthex64 を直に呼ぶ** */
void puthex(uint64_t value) {
    static const char digits[] = "0123456789abcdef";
    char buf[17];
    int i;
    for (i = 0; i < 16; i++) buf[i] = digits[(value >> ((15 - i) * 4)) & 0xfU];
    buf[16] = '\0';
    puts(buf);
}

int64_t sys_write_serial(const char* buf, size_t count) {
    extern int arch_console_onlcr_enabled(void);   /* kernel/linux_syscall.c */
    int onlcr;
    if (!buf) return -1;
    /* **termios の ONLCR を開く。** 実機のシリアル端末は LF だけでは行頭に
     * 戻らない (Pi 4 の ash で `ls` の段組みが階段状に崩れて発覚)。
     * QEMU の -serial stdio ではホスト端末が吸収するので見えない。
     *
     * **既に CR が置かれている所には足さない。** 共有層のコンソールエコーは
     * "\r\n" を渡してくるので、無条件に足すと CR が 2 つ出る */
    onlcr = arch_console_onlcr_enabled();
    aarch64_console_begin();
    for (size_t i = 0; i < count; i++) {
        if (onlcr && buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            aarch64_uart_putchar('\r');
        }
        aarch64_uart_putchar(buf[i]);
    }
    aarch64_console_end();
    return (int64_t)count;
}

void kernel_panic(const char* file, int line, const char* func, const char* expr) {
    aarch64_console_begin();
    puts("\n*** KERNEL PANIC ***\n");
    puts("expr: "); puts(expr ? expr : "(null)");
    puts("\nfunc: "); puts(func ? func : "(null)");
    puts("\nfile: "); puts(file ? file : "(null)");
    puts(":"); aarch64_uart_putdec64((uint64_t)(uint32_t)line);
    puts("\nHALTING...\n");
    aarch64_console_end();
    for (;;) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

/* ==========================================================================
 * 共有層の自己診断 (M3c-2a)
 *
 * **コンパイルが通ったことは、動くことの証拠にならない。** ここで確かめる
 * のは「共有層が実際に使う経路」だけ:
 *
 *   1. pmm.h の形で確保できて、PHYS_TO_VIRT で触れること
 *      → g_hhdm_offset が効いている証拠。0 のままなら物理を返して落ちる
 *   2. 参照カウントが効くこと
 *      → 1 回目の pmm_free で返してしまうと、まだ使われているページを配る
 *   3. arch_vm_* でアドレス空間を作って張って外せること
 *      → 張った直後に get_phys が同じ物理を返し、外した後は 0 を返す
 *   4. **後始末でページ数が元に戻ること**
 *      → 戻らなければどこかで漏れている
 * ========================================================================== */
#include "pmm.h"
#include "vmm.h"
#include "arch_vm.h"

/* どこにも使われていない VA。**新しく作った空間なので何でもよい**が、
 * ユーザーのコードが居る 0x400000 とは離しておく */
#define SELFTEST_VA  0x0000000010000000ULL

static void report(const char* label, int ok, int* all) {
    aarch64_console_begin();
    puts(label);
    puts(ok ? "  ok\n" : "  BAD\n");
    aarch64_console_end();
    if (!ok) *all = 0;
}

int aarch64_shared_layer_selftest(void) {
    int ok = 1;
    uint64_t pages_before = pmm_get_allocated_pages();
    uint64_t pages_after;
    void* page;
    volatile uint64_t* p;
    arch_address_space_t as;
    uint64_t got;

    aarch64_console_begin();
    puts("--- M3c-2a: 共有層 (pmm.h / arch_vm_*) ---\n");
    aarch64_console_end();

    /* 1. 確保して PHYS_TO_VIRT 経由で触る */
    page = pmm_alloc(1);
    report("  pmm_alloc :", page != 0, &ok);
    if (!page) goto done;

    p = (volatile uint64_t*)PHYS_TO_VIRT(page);
    *p = 0x5aa5c33c5aa5c33cULL;
    report("  hhdm rw   :", *p == 0x5aa5c33c5aa5c33cULL, &ok);

    /* 2. 参照カウント。**1 回目の free では返らないこと**まで見る */
    pmm_incref(page);
    report("  incref    :", pmm_get_ref(page) == 2, &ok);
    pmm_free(page, 1);
    report("  free x1   :", pmm_get_ref(page) == 1, &ok);

    /* 3. アドレス空間を作って張って外す */
    as = arch_vm_create_user_address_space();
    report("  create as :", as != 0, &ok);
    if (as) {
        arch_vm_map_page(as, SELFTEST_VA, (uint64_t)(uintptr_t)page,
                         arch_vm_user_page_flags(1, 0));
        got = arch_vm_get_phys(as, SELFTEST_VA);
        report("  map/get   :", got == (uint64_t)(uintptr_t)page, &ok);

        arch_vm_unmap_page(as, SELFTEST_VA);
        got = arch_vm_get_phys(as, SELFTEST_VA);
        report("  unmap     :", got == 0, &ok);

        arch_vm_destroy_user_address_space(as);
    }

    /* 4. 後始末。**確保したぶんが戻ること。**
     * 上の arch_vm_destroy_user_address_space が、arch_vm_map_page の作った
     * 中間テーブルまで返すようになった (P3-4)。残るものは無い */
    pmm_free(page, 1);
    report("  free x2   :", pmm_get_ref(page) == 0, &ok);

done:
    pages_after = pmm_get_allocated_pages();
    aarch64_console_begin();
    puts("  pages     : ");
    aarch64_uart_putdec64(pages_before);
    puts(" -> ");
    aarch64_uart_putdec64(pages_after);
    /* **P3-4 で「完全に戻る」に変わった。**
     *
     * それまでは arch_vm_destroy_user_address_space が root 1 枚しか返さず、
     * 張るのに使った L2 / L3 の 2 枚が残るので +2 を期待値にしていた。
     * いまは中間テーブルもページ本体も返すので **前後で一致する。**
     *
     * 前の判定は「ここが変わったら気づける」と書いてあり、実際に
     * aarch64-shared-BAD で気づけた。**期待値は実態に合わせて更新すること** —
     * 「まだ出来ていない」を前提にした数字を残すと、直したときに落ちる */
    puts(pages_after == pages_before ? "  ok (確保したぶんが全部戻った)\n"
                                     : "  BAD (見込みと違う)\n");
    if (pages_after != pages_before) ok = 0;
    puts(ok ? "aarch64-shared-ok\n" : "aarch64-shared-BAD\n");
    aarch64_console_end();
    return ok;
}

/* ==========================================================================
 * SMP (M3c-2b) — **CPU 1 本しか無い**
 *
 * riscv64 は SBI HSM で副 hart を起こす (kernel/riscv64/smp.c 187 行)。
 * aarch64 は PSCI (HVC/SMC 経由) を使うことになるが、まだ入れていない。
 * ここでは「1 本しか無い」を正しく答えるだけにする。
 *
 * **0 を返して誤魔化さない。** cpu 0 の情報は本物を返し、範囲外は 0。
 * ========================================================================== */
#include "smp.h"
#include "task.h"

static struct smp_cpu_info g_cpus[1];
static int g_cpus_ready;

const struct smp_cpu_info* smp_get_cpu_info(uint32_t cpu_index) {
    if (cpu_index != 0) return 0;
    if (!g_cpus_ready) {
        g_cpus[0].cpu_index = 0;
        g_cpus[0].processor_id = 0;
        g_cpus[0].lapic_id = 0;
        g_cpus[0].is_bsp = 1;
        g_cpus[0].started = 1;
        g_cpus_ready = 1;
    }
    return &g_cpus[0];
}

uint32_t smp_get_started_cpu_count(void) { return 1; }

/* **自分にしか送りようが無い。** 自 CPU への resched は
 * task_request_resched がフラグを立てるので、ここで何かする必要は無い。
 * 他 CPU が現れたら PSCI と GIC の SGI が要る */
void smp_send_resched_ipi(uint32_t cpu_id) {
    (void)cpu_id;
}

/* コンソールの fd は kernel/fs.c の本物を使う (C-1a)。
 * M3c-2b では -1 を返すスタブを置いていたが、fs.c を取り込んだので外した */

/* ---- 譲る (M3c-2b) -------------------------------------------------------
 *
 * riscv64 と同じく、**BKL を全部手放してから schedule()、戻ったら取り直す。**
 * 持ったまま切り替えると、次のタスクが同じロックを待って進めなくなる */
void kernel_yield(void) {
    uint32_t depth = g_kernel_lock_depth;
    for (uint32_t i = 0; i < depth; i++) kernel_lock_exit();
    schedule();
    for (uint32_t i = 0; i < depth; i++) kernel_lock_enter();
}

/* ==========================================================================
 * 共有タスク層の自己診断 (M3c-2b)
 *
 * **リンクが通ったことは、動く証拠にならない。** 確かめるのは
 * 「共有スケジューラで実際に切り替わって戻ってくる」ことだけ。
 *
 * 寝て起きる 1 往復に、確かめたいものが全部入っている:
 *
 *   schedule()            走行中のタスクを降ろして idle を選ぶ
 *   arch_context_switch   aarch64_context_switch に繋がっている
 *   cpu_local             current_task / idle_task が引ける
 *   task_on_timer_tick    タイマ割り込みが共有層に届いている
 *   task_poll_sleep_wakeups  期限が来たタスクを起こす
 *   task_consume_resched  IRQ の出口で schedule() が呼ばれる
 *
 * **どれか 1 つでも欠けたら戻ってこない。** 逆に言えば、戻ってきた時点で
 * 全部繋がっている。経過時間を見るのは「即座に戻ってきた」= 切り替えて
 * いない場合を弾くため。
 * ========================================================================== */
#define SHARED_SLEEP_MS 50

int aarch64_shared_task_selftest(void) {
    struct task* cur;
    uint64_t t0, t1, elapsed;
    int ok = 1;

    aarch64_console_begin();
    puts("--- M3c-2b: 共有スケジューラ (kernel/sched.c) ---\n");
    aarch64_console_end();

    /* **乗り換えは task_init の前。** 後にすると、その隙のタイマ割り込みが
     * M3c-1 の器を叩き、まだ器のタスクが居る前提で切り替えようとする */
    aarch64_task_use_shared_scheduler();
    task_init();

    cur = get_current_task();
    report("  current   :", cur != 0, &ok);
    if (!cur) goto done;

    report("  cpu local :", get_cpu_local() != 0, &ok);
    report("  idle task :", get_cpu_local() && get_cpu_local()->idle_task != 0, &ok);

    /* **ここが本体。** 期限付きで寝て、タイマに起こしてもらう */
    t0 = arch_time_now_ms();
    task_mark_io_wait_until(cur, t0 + SHARED_SLEEP_MS);
    kernel_yield();
    t1 = arch_time_now_ms();
    elapsed = t1 - t0;

    aarch64_console_begin();
    puts("  sleep     : ");
    aarch64_uart_putdec64(elapsed);
    /* 即座に戻ってきたら切り替えていない。**しきい値は境界に乗せない** —
     * 要求 50ms に対して 40ms 以上を合格とする (日報2026-08-09 追2-3) */
    puts(elapsed >= (SHARED_SLEEP_MS - 10) ? " ms  ok (寝て、タイマに起こされた)\n"
                                           : " ms  BAD (切り替わっていない)\n");
    aarch64_console_end();
    if (elapsed < (SHARED_SLEEP_MS - 10)) ok = 0;

done:
    aarch64_console_begin();
    puts(ok ? "aarch64-sched-ok\n" : "aarch64-sched-BAD\n");
    aarch64_console_end();
    return ok;
}

/* ==========================================================================
 * M4-3: virtio-blk を storage 層に登録して xv6fs をマウントする
 *
 * **task_init の後に呼ぶこと。** xv6fs は sleeplock (wait_event) を使うので、
 * 待てるタスクが居ないと成立しない。riscv64 も同じ順序。
 *
 * 確かめるのは 4 つ:
 *   1. マウントできること
 *   2. **イメージに入れておいた既知のファイルが読めること**
 *      マウントできただけでは、中身を辿れる証拠にならない
 *   3. 新しいファイルを書いて読み戻せること (ログと bitmap が効いている)
 *   4. 書いたものが**再マウントなしで**見えること
 * ========================================================================== */
#include "storage.h"
#include "xv6fs.h"
#include "fs.h"

/* スモークがイメージに入れておくファイル。**中身まで照合する。**
 * 「読めた」だけでは、別のブロックを返していても気づけない */
#define FS_PROBE_PATH  "/aarch64-m4.txt"
#define FS_PROBE_TEXT  "ORTHOX-AARCH64-XV6FS-OK"
#define FS_WRITE_PATH  "/written-by-kernel.txt"
#define FS_WRITE_TEXT  "written-by-aarch64-kernel"
/* VFS / fd 層を通す経路の確認 (C-1a)。**xv6fs 直叩きとは別の道** */
#define FS_FD_PATH     "/via-vfs.txt"
#define FS_FD_TEXT     "through-fs_open-and-fs_write"

static int fs_streq(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

static unsigned fs_strlen(const char* s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

/* path を読んで buf に入れる。戻り値は読めたバイト数、負なら失敗 */
static int fs_read_path(const char* path, char* buf, unsigned cap) {
    struct xv6fs_inode* ip = xv6fs_namei(path);
    int n;
    if (!ip) return -1;
    xv6fs_ilock(ip);
    n = xv6fs_readi(ip, buf, 0, cap - 1);
    xv6fs_iunlockput(ip);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

static char g_fs_buf[128];

int aarch64_fs_selftest(void) {
    int ok = 1;
    int n;
    const char* disk_name;

    aarch64_console_begin();
    puts("--- M4-3: xv6fs (virtio-blk / EMMC2 の上) ---\n");
    aarch64_console_end();

    /* **fs_init が storage_init も呼ぶ。** ramfs / fifo / VFS の
     * マウントテーブルもここで初期化される (C-1a) */
    fs_init();

    /* **どのストレージで動くかは機械で変わる。**
     * QEMU virt は virtio-blk、Raspberry Pi 4 は EMMC2 (SD カード)。
     * 上の層 (xv6bio / xv6fs / VFS) は storage の名前しか見ないので、
     * ここで登録するものを差し替えるだけで済む */
    if (aarch64_virtio_blk_present()) {
        if (storage_register_device("vblk0", 512, aarch64_virtio_blk_capacity(),
                                    aarch64_virtio_blk_storage_read,
                                    aarch64_virtio_blk_storage_write, 0, 0) < 0) {
            report("  register  :", 0, &ok);
            goto done;
        }
        disk_name = "vblk0";
    } else if (aarch64_emmc2_init() == 0) {
        if (storage_register_device("sd0", 512, aarch64_emmc2_blocks(),
                                    aarch64_emmc2_storage_read,
                                    aarch64_emmc2_storage_write, 0, 0) < 0) {
            report("  register  :", 0, &ok);
            goto done;
        }
        disk_name = "sd0";
    } else {
        /* ディスクを付けずに起動した回。**「無い」と「壊れた」を混ぜない** */
        aarch64_console_begin();
        puts("  device    : 見つからない (-drive を付けずに起動した)\n");
        puts("aarch64-fs-none\n");
        aarch64_console_end();
        return 1;
    }
    report("  register  :", 1, &ok);

    report("  mount     :", xv6fs_mount_storage(disk_name) == 0, &ok);
    if (!xv6fs_is_mounted()) goto done;

    /* **root を xv6fs に切り替える。** これを忘れると fs.c は
     * ROOT_SOURCE_MODULE のまま探しに行き、exec が「File not found」に
     * なる (実際に踏んだ)。マウントできたことと、root として使うことは別 */
    report("  root=xv6fs:", fs_mount_xv6fs_root() == 0, &ok);

    /* 2. イメージに入れておいた既知のファイル。**中身まで照合する** */
    n = fs_read_path(FS_PROBE_PATH, g_fs_buf, sizeof(g_fs_buf));
    report("  read file :", n > 0 && fs_streq(g_fs_buf, FS_PROBE_TEXT,
                                              sizeof(FS_PROBE_TEXT)), &ok);

    /* 3. 書いて読み戻す。**ログと bitmap が効いていないとここで落ちる** */
    {
        struct xv6fs_inode* ip = 0;
        int rc = xv6fs_create_file(FS_WRITE_PATH, 0644, &ip);
        if (ip) xv6fs_iunlockput(ip);
        report("  create    :", rc == 0, &ok);
        if (rc == 0) {
            /* **xv6fs_write_file は成功で 0 を返す。** バイト数ではない。
             * 最初 strlen と比べて BAD を出したが、直後の read back が
             * ok だったので「コードではなく判定が間違っている」と分かった。
             * **戻り値の約束は推測せず、呼ぶ先を読んで確かめる** */
            rc = xv6fs_write_file(FS_WRITE_PATH, 0, FS_WRITE_TEXT,
                                  fs_strlen(FS_WRITE_TEXT));
            report("  write     :", rc == 0, &ok);
            n = fs_read_path(FS_WRITE_PATH, g_fs_buf, sizeof(g_fs_buf));
            report("  read back :", n == (int)fs_strlen(FS_WRITE_TEXT) &&
                                    fs_streq(g_fs_buf, FS_WRITE_TEXT,
                                             sizeof(FS_WRITE_TEXT)), &ok);
        }
    }

    /* 4. **VFS / fd 層を通す (C-1a)。**
     *
     * ここまでは xv6fs を直接叩いていた。共有層の fs_open / fs_write /
     * fs_read / fs_close は、**タスクの fd テーブルとマウントの解決**を
     * 挟む。ユーザープログラムが通るのはこちらの経路なので、
     * 直叩きが通ったことは、こちらが通る証拠にならない */
    {
        int fd = fs_open(FS_FD_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
        int64_t n64;
        report("  fd open   :", fd >= 0, &ok);
        if (fd >= 0) {
            n64 = fs_write(fd, FS_FD_TEXT, fs_strlen(FS_FD_TEXT));
            report("  fd write  :", n64 == (int64_t)fs_strlen(FS_FD_TEXT), &ok);
            report("  fd close  :", fs_close(fd) == 0, &ok);
        }
        /* **開き直して読む。** 同じ fd のバッファから読み返しただけでは、
         * ディスクに届いた証拠にならない */
        fd = fs_open(FS_FD_PATH, O_RDONLY, 0);
        if (fd >= 0) {
            for (unsigned i = 0; i < sizeof(g_fs_buf); i++) g_fs_buf[i] = 0;
            n64 = fs_read(fd, g_fs_buf, sizeof(g_fs_buf) - 1);
            report("  fd read   :", n64 == (int64_t)fs_strlen(FS_FD_TEXT) &&
                                    fs_streq(g_fs_buf, FS_FD_TEXT,
                                             sizeof(FS_FD_TEXT)), &ok);
            fs_close(fd);
        } else {
            report("  fd read   :", 0, &ok);
        }
    }

    /* 5. **タスクのコンソール fd が本物になったこと。**
     * M3c-2b までは fs_init_console_fd が -1 を返すスタブだった */
    {
        struct task* cur = get_current_task();
        report("  console fd:", cur && cur->fds[1].in_use, &ok);
    }

done:
    aarch64_console_begin();
    puts(ok ? "aarch64-fs-ok\n" : "aarch64-fs-BAD\n");
    aarch64_console_end();
    return ok;
}

/* ==========================================================================
 * P1: 最初のユーザープロセス
 *
 * **ディスクの ELF を読んで EL0 で走らせる。** カーネルに埋め込まない
 * (riscv64 は埋め込みの器も持っているが、こちらは ELF の読み込み経路ごと
 *  確かめたいので最初からディスクから読む)。
 *
 * riscv64 の riscv64_first_user_task_bootstrap_continue と同じ形:
 *   task_execve(&frame, path, argv, envp) -> task_main()
 * ========================================================================== */

int task_execve(arch_task_exec_frame_t* frame, const char* path,
                char* const argv[], char* const envp[]);
void task_main(void);

/* 既定は P1 の最小プログラム。P2 のスモークは Makefile から
 * -DAARCH64_INIT_PATH=/bin/musl-probe を渡して差し替える */
#ifndef AARCH64_INIT_PATH
#define AARCH64_INIT_PATH "/bin/hello"
#endif

int aarch64_first_user_task(void) {
    static char* argv[] = { (char*)AARCH64_INIT_PATH, 0 };
    static char* envp[] = { 0 };
    arch_task_exec_frame_t frame;

    aarch64_console_begin();
    puts("--- P1: 最初のユーザープロセス ---\n");
    puts("  exec      : " AARCH64_INIT_PATH "\n");
    aarch64_console_end();

    if (!get_current_task()) {
        puts("  BAD (current task がいない)\naarch64-init-BAD\n");
        return 0;
    }

    /* **svc をここで共有層に切り替える。** M3a の自前検査より後にする
     * (あちらは独自の write/exit を前提にしている) */
    aarch64_use_shared_syscalls_on();

    if (task_execve(&frame, AARCH64_INIT_PATH, argv, envp) < 0) {
        puts("  exec      : BAD (execve が失敗)\naarch64-init-BAD\n");
        return 0;
    }

    /* **戻ってこない。** EL0 へ降り、exit したらゾンビになって schedule へ */
    task_main();
    return 1;
}

/* ---- まだ無いもの --------------------------------------------------------
 *
 * **黙って成功を返さない。** 「何もしない」で正しいものだけをここに置く。
 * ネットワークは aarch64 に無いので、ポーリングの必要も無い */
int bottom_half_run(void) {
    return 0;
}

int net_needs_poll_fallback(void) {
    return 0;
}

void net_poll(void) {
}
