/*
 * AArch64 の早期起動。
 *
 * **アドレスは直書きしない。DTB から取る (M2b)。** 直書きの既定値は
 * include/aarch64/boot.h に QEMU virt の実測値として置いてあるが、これは
 * DTB が読めなかったときに退く先であって、Raspberry Pi 4 では全部違う。
 *
 * ここで表示する CurrentEL は決め打ちにしない。QEMU virt は既定
 * (virtualization=off) だと EL1、virtualization=on だと EL2 で始まる。
 * どちらで来ているかは M1 (例外ベクタ / EL2->EL1 の降格) の前提になる。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/dtb.h"

/* PL011 UART。QEMU は初期化なしでも DR に書けば出るが、実機と手順を
 * 揃えておく (M1 以降で割り込み受信を足すときに効く)
 *
 * **DTB を読む前に UART が要る**という循環がある (解析結果を表示したい)。
 * 既定値で出し始め、DTB が別の場所を指していたら差し替える。
 * Pi 4 の PL011 は 0xFE201000 なので、**実機では差し替え前の出力が
 * 捨てられる**。段取り 3 で既定値の決め方が要る。 */
static uint64_t g_uart_base = AARCH64_QEMU_VIRT_UART0_BASE;

#define PL011_DR_OFF    0x00
#define PL011_FR_OFF    0x18
#define PL011_FR_TXFF   (1U << 5)   /* 送信 FIFO が満杯 */

static inline void mmio_write32(uint64_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = v;
}

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

void aarch64_uart_set_base(uint64_t base) {
    if (base) g_uart_base = base;
}

void aarch64_uart_putchar(char c) {
    while (mmio_read32(g_uart_base + PL011_FR_OFF) & PL011_FR_TXFF) {
        /* 送信 FIFO が空くまで待つ */
    }
    mmio_write32(g_uart_base + PL011_DR_OFF, (uint32_t)(unsigned char)c);
}

void aarch64_uart_puts(const char* s) {
    if (!s) return;
    while (*s) {
        if (*s == '\n') aarch64_uart_putchar('\r');
        aarch64_uart_putchar(*s);
        s++;
    }
}

/* M2 以降 vm.c からも使うので外に出してある */
void aarch64_uart_puthex64(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xfU];
    }
    buf[18] = '\0';
    aarch64_uart_puts(buf);
}

static void put_hex64(uint64_t v) {
    aarch64_uart_puthex64(v);
}

static uint64_t read_current_el(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    return v >> 2;   /* [3:2] が EL */
}

static uint64_t read_mpidr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(v));
    return v;
}

extern char aarch64_vectors[];

void aarch64_gic_init(void);
void aarch64_gic_enable_irq(unsigned intid);
uint32_t aarch64_gic_claim(void);
void aarch64_gic_complete(uint32_t iar);
void aarch64_timer_init(void);
void aarch64_timer_on_tick(void);
uint64_t aarch64_timer_freq(void);
uint64_t aarch64_timer_ticks(void);
uint32_t aarch64_timer_intid(void);
void aarch64_vm_init(void);
int aarch64_user_run(void);

/* MMU の探針 (vm.c)。「未マップの VA を読んだら fault が上がるはず」の
 * やりとりに使う。上がった fault はここで拾って呼び出し元に返す */
extern volatile int g_aarch64_vm_expect_fault;
extern volatile uint64_t g_aarch64_vm_fault_esr;

static void aarch64_vectors_init(void) {
    __asm__ volatile("msr vbar_el1, %0" :: "r"((uint64_t)aarch64_vectors));
    __asm__ volatile("isb");
}

/* ---- 起動時に確定させる情報 ---------------------------------------------
 *
 * まず直書きの既定値 (QEMU virt の実測値) で埋め、そのうえで DTB が読めた
 * ぶんだけ上書きする。**どちらから来た値かを flags で持つ**ので、
 * 「DTB を読んだつもりで既定値のままだった」を見分けられる。 */
static aarch64_boot_info_t g_boot_info;

aarch64_boot_info_t* aarch64_boot_info_mut(void) { return &g_boot_info; }
const aarch64_boot_info_t* aarch64_boot_info(void) { return &g_boot_info; }

void aarch64_boot_capture(uint64_t dtb_hint) {
    uint64_t dtb;

    /* memory_size と virtio_mmio_stride だけは 0 のまま走査に入る。
     * 走査側が「まだ埋まっていない」を 0 で判断するため。既定値は後で入れる */
    g_boot_info.uart_base   = AARCH64_QEMU_VIRT_UART0_BASE;
    g_boot_info.gicd_base   = AARCH64_QEMU_VIRT_GICD_BASE;
    g_boot_info.gicd_size   = AARCH64_QEMU_VIRT_GIC_SIZE;
    g_boot_info.gicc_base   = AARCH64_QEMU_VIRT_GICC_BASE;
    g_boot_info.gicc_size   = AARCH64_QEMU_VIRT_GIC_SIZE;
    g_boot_info.first_virtio_mmio_base = AARCH64_QEMU_VIRT_VIRTIO_BASE;
    g_boot_info.timer_intid = AARCH64_TIMER_PPI_DEFAULT + AARCH64_PPI_INTID_BASE;

    dtb = aarch64_dtb_find(dtb_hint);
    if (dtb) {
        g_boot_info.dtb_pa = dtb;
        g_boot_info.dtb_size = aarch64_dtb_total_size(dtb);
        g_boot_info.flags |= AARCH64_BOOT_FLAG_DTB_VALID;
        g_boot_info.flags |= (dtb == dtb_hint) ? AARCH64_BOOT_FLAG_DTB_FROM_X0
                                               : AARCH64_BOOT_FLAG_DTB_FROM_SCAN;
        aarch64_dtb_scan(dtb);
    }

    if (g_boot_info.memory_size == 0) {
        g_boot_info.memory_base = AARCH64_QEMU_VIRT_RAM_BASE;
        g_boot_info.memory_size = AARCH64_QEMU_VIRT_RAM_SIZE;
    }
    if (g_boot_info.virtio_mmio_stride == 0) {
        g_boot_info.virtio_mmio_stride = AARCH64_QEMU_VIRT_VIRTIO_STRIDE;
    }

    /* UART が別の場所だと分かったらここで乗り換える */
    aarch64_uart_set_base(g_boot_info.uart_base);
}

/* 1 行 1 項目で出す。**どこから来た値かを添える。** 「DTB を読んだ」と
 * 「DTB の値を使っている」は別のことなので、区別できる形にしておく */
static void put_src(uint32_t flag) {
    aarch64_uart_puts((g_boot_info.flags & flag) ? "  (dtb)\n" : "  (既定値)\n");
}

static void aarch64_boot_info_dump(void) {
    const aarch64_boot_info_t* b = &g_boot_info;

    aarch64_uart_puts("--- M2b: DTB ---\n");

    aarch64_uart_puts("  dtb       : ");
    if (b->flags & AARCH64_BOOT_FLAG_DTB_VALID) {
        put_hex64(b->dtb_pa);
        aarch64_uart_puts(" size ");
        put_hex64(b->dtb_size);
        aarch64_uart_puts((b->flags & AARCH64_BOOT_FLAG_DTB_FROM_X0)
                          ? "  (x0)\n" : "  (走査)\n");
    } else {
        aarch64_uart_puts("見つからない。直書きの既定値で進む\n");
    }

    aarch64_uart_puts("  memory    : ");
    put_hex64(b->memory_base);
    aarch64_uart_puts(" size ");
    put_hex64(b->memory_size);
    put_src(AARCH64_BOOT_FLAG_MEMORY_FROM_DTB);

    aarch64_uart_puts("  uart      : ");
    put_hex64(b->uart_base);
    put_src(AARCH64_BOOT_FLAG_UART_FROM_DTB);

    /* **2 組そろって初めて GIC として使える。** 1 組目だけ取れても
     * (dtb) とは言わない (日報2026-08-09 §1 で落とした所) */
    aarch64_uart_puts("  gic dist  : ");
    put_hex64(b->gicd_base);
    aarch64_uart_puts(" size ");
    put_hex64(b->gicd_size);
    put_src(AARCH64_BOOT_FLAG_GIC_FROM_DTB);
    aarch64_uart_puts("  gic cpu   : ");
    put_hex64(b->gicc_base);
    aarch64_uart_puts(" size ");
    put_hex64(b->gicc_size);
    put_src(AARCH64_BOOT_FLAG_GIC_FROM_DTB);

    aarch64_uart_puts("  virtio    : ");
    put_hex64(b->first_virtio_mmio_base);
    aarch64_uart_puts(" x ");
    put_hex64(b->virtio_mmio_count);
    aarch64_uart_puts(" stride ");
    put_hex64(b->virtio_mmio_stride);
    put_src(AARCH64_BOOT_FLAG_VIRTIO_FROM_DTB);

    aarch64_uart_puts("  timer irq : ");
    put_hex64(b->timer_intid);
    put_src(AARCH64_BOOT_FLAG_TIMER_FROM_DTB);

    aarch64_uart_puts("  cpus      : ");
    put_hex64(b->cpu_count);
    aarch64_uart_puts("\n");

    if (b->flags & AARCH64_BOOT_FLAG_DTB_VALID) {
        aarch64_uart_puts("aarch64-dtb-ok\n");
    } else {
        aarch64_uart_puts("aarch64-dtb-BAD (DTB が見つからない)\n");
    }
}

void aarch64_wait_forever(void);

void aarch64_early_main(uint64_t dtb_phys) {
    aarch64_uart_puts("\n--- Orthox-64 aarch64 boot ---\n");

    aarch64_uart_puts("  CurrentEL : EL");
    aarch64_uart_putchar((char)('0' + (int)(read_current_el() & 3U)));
    aarch64_uart_puts("\n");

    aarch64_uart_puts("  MPIDR_EL1 : ");
    put_hex64(read_mpidr());
    aarch64_uart_puts("\n");

    aarch64_uart_puts("  x0 (DTB?) : ");
    put_hex64(dtb_phys);
    aarch64_uart_puts("\n");

    /* .bss が実際に 0 で埋まっているかを見る。start.S の埋め方を間違えると
     * ここから先の C が静かに壊れるので、先に確かめておく */
    {
        static uint64_t bss_probe;
        aarch64_uart_puts("  bss zero  : ");
        aarch64_uart_puts(bss_probe == 0 ? "ok" : "BAD");
        aarch64_uart_puts("\n");
    }

    aarch64_uart_puts("aarch64-boot-ok\n");

    /* ---- M2b: DTB -------------------------------------------------------
     *
     * **MMU より先に置く。** vm.c は DTB が言う RAM とデバイスの位置を
     * 見てマップするので、ここで確定していないと直書きのままになる */
    aarch64_boot_capture(dtb_phys);
    aarch64_boot_info_dump();

    /* ---- M1: 例外ベクタ + GIC + generic timer -------------------------- */
    aarch64_vectors_init();
    aarch64_gic_init();
    aarch64_gic_enable_irq(aarch64_timer_intid());
    aarch64_timer_init();

    aarch64_uart_puts("  timer freq: ");
    put_hex64(aarch64_timer_freq());
    aarch64_uart_puts("\n");

    /* 割り込みを通す (DAIF の I ビットを落とす)。riscv64 の sstatus.SIE 相当 */
    __asm__ volatile("msr daifclr, #2");

    /* tick が入ることを確かめる。入らなければここで止まったままになるので、
     * 「動いた」と「止まった」が区別できる */
    {
        uint64_t want = 10;     /* 10ms x 10 = 100ms ぶん */
        uint64_t spin = 0;
        while (aarch64_timer_ticks() < want) {
            /* wfi で寝ると割り込みで起きる。空回しより素直 */
            __asm__ volatile("wfi");
            if (++spin > 1000) break;   /* 保険。無限に待たない */
        }
        aarch64_uart_puts("  ticks     : ");
        put_hex64(aarch64_timer_ticks());
        aarch64_uart_puts("\n");
        if (aarch64_timer_ticks() >= want) {
            aarch64_uart_puts("aarch64-timer-ok\n");
        } else {
            aarch64_uart_puts("aarch64-timer-BAD (tick が入らない)\n");
        }
    }

    /* ---- M2: MMU ------------------------------------------------------- */
    aarch64_vm_init();

    /* MMU を入れた後もタイマが入り続けるかを見る。**ここが本命の確認。**
     * GIC を Device 属性で張り忘れていたり、ベクタ表のあるページが張れて
     * いなければ、tick が止まるか例外で沈黙する */
    {
        uint64_t before = aarch64_timer_ticks();
        uint64_t want = before + 5;
        uint64_t spin = 0;
        while (aarch64_timer_ticks() < want) {
            __asm__ volatile("wfi");
            if (++spin > 1000) break;
        }
        aarch64_uart_puts("  post ticks: ");
        put_hex64(aarch64_timer_ticks());
        aarch64_uart_puts("\n");
        if (aarch64_timer_ticks() >= want) {
            aarch64_uart_puts("aarch64-mmu-ok\n");
        } else {
            aarch64_uart_puts("aarch64-mmu-BAD (MMU on で tick が止まった)\n");
        }
    }

    /* ---- M3a: EL0 に降りて svc で戻る ---------------------------------- */
    aarch64_user_run();

    aarch64_wait_forever();
}

/* 例外ベクタから呼ばれる。IRQ の入口 */
void aarch64_irq_handler(void) {
    uint32_t iar = aarch64_gic_claim();
    uint32_t intid = iar & 0x3ffU;

    if (intid == aarch64_timer_intid()) {
        aarch64_timer_on_tick();
    }
    /* 1023 は「上がっていなかった」を意味する偽物。EOI してはいけない */
    if (intid < 1020U) {
        aarch64_gic_complete(iar);
    }
}

/* カーネル実行中の同期例外 (ベクタ +0x200)。M1 までは無条件に止めていたが、
 * M2 で「わざと未マップの VA を読んで fault を確かめる」探針を入れたので、
 * 想定内のものだけ復帰できるようにする。
 *
 * 戻り値 0 = 復帰しない (止まる) / 1 = 例外を起こした命令の次から再開。
 * **既定は 0。** 想定外の例外を黙って読み飛ばすと、原因不明の暴走になる */
int aarch64_sync_exception(uint64_t esr, uint64_t elr, uint64_t far) {
    if (g_aarch64_vm_expect_fault) {
        g_aarch64_vm_expect_fault = 0;
        g_aarch64_vm_fault_esr = esr;
        return 1;
    }

    aarch64_uart_puts("\n*** aarch64 sync exception ***\n");
    aarch64_uart_puts("  ESR    : ");
    put_hex64(esr);
    aarch64_uart_puts("\n  ELR    : ");
    put_hex64(elr);
    aarch64_uart_puts("\n  FAR    : ");
    put_hex64(far);
    aarch64_uart_puts("\naarch64-exception-BAD\n");
    return 0;
}

/* 想定していない例外。**黙って戻らない。** 取りこぼすと原因不明のハングに
 * なるので、どの入口で何が起きたかを出してから止める */
void aarch64_unexpected_exception(uint64_t which, uint64_t esr, uint64_t elr) {
    aarch64_uart_puts("\n*** aarch64 unexpected exception ***\n");
    aarch64_uart_puts("  vector : ");
    put_hex64(which);
    aarch64_uart_puts("\n  ESR    : ");
    put_hex64(esr);
    aarch64_uart_puts("\n  ELR    : ");
    put_hex64(elr);
    aarch64_uart_puts("\naarch64-exception-BAD\n");
}
