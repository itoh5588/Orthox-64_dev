/*
 * AArch64 (QEMU virt) の早期起動。M0: PL011 に文字が出るところまで。
 *
 * アドレスは QEMU が吐いた DTB から取った実測値 (推測ではない):
 *
 *   memory@40000000   0x40000000  size 0x20000000 (512MB)
 *   pl011@9000000     0x09000000  compatible = arm,pl011
 *   intc@8000000      0x08000000  compatible = arm,cortex-a15-gic (GICv2)
 *   virtio_mmio@...   0x0a000000  0x200 刻みで 32 スロット
 *
 * ここで表示する CurrentEL は決め打ちにしない。QEMU virt は既定
 * (virtualization=off) だと EL1、virtualization=on だと EL2 で始まる。
 * どちらで来ているかは M1 (例外ベクタ / EL2->EL1 の降格) の前提になる。
 */
#include <stdint.h>

/* PL011 UART。QEMU は初期化なしでも DR に書けば出るが、実機と手順を
 * 揃えておく (M1 以降で割り込み受信を足すときに効く) */
#define PL011_BASE  0x09000000UL
#define PL011_DR    (PL011_BASE + 0x00)
#define PL011_FR    (PL011_BASE + 0x18)
#define PL011_FR_TXFF  (1U << 5)   /* 送信 FIFO が満杯 */

static inline void mmio_write32(uint64_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = v;
}

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

void aarch64_uart_putchar(char c) {
    while (mmio_read32(PL011_FR) & PL011_FR_TXFF) {
        /* 送信 FIFO が空くまで待つ */
    }
    mmio_write32(PL011_DR, (uint32_t)(unsigned char)c);
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

/* 非セキュア物理タイマの割り込み番号。DTB の timer interrupts が
 * <1 14 0x104> = PPI 番号 14 で、PPI の INTID は +16 なので 30 */
#define AARCH64_IRQ_TIMER 30

extern char aarch64_vectors[];

void aarch64_gic_init(void);
void aarch64_gic_enable_irq(unsigned intid);
uint32_t aarch64_gic_claim(void);
void aarch64_gic_complete(uint32_t iar);
void aarch64_timer_init(void);
void aarch64_timer_on_tick(void);
uint64_t aarch64_timer_freq(void);
uint64_t aarch64_timer_ticks(void);
void aarch64_vm_init(void);

/* MMU の探針 (vm.c)。「未マップの VA を読んだら fault が上がるはず」の
 * やりとりに使う。上がった fault はここで拾って呼び出し元に返す */
extern volatile int g_aarch64_vm_expect_fault;
extern volatile uint64_t g_aarch64_vm_fault_esr;

static void aarch64_vectors_init(void) {
    __asm__ volatile("msr vbar_el1, %0" :: "r"((uint64_t)aarch64_vectors));
    __asm__ volatile("isb");
}

/* QEMU に ELF を -kernel で渡すと x0 に DTB のアドレスが来ない (実測。
 * -dtb を足しても変わらない)。Linux の boot protocol は Image 形式向けで、
 * ELF は「素のバイナリ」として扱われるため。
 *
 * DTB 自体はメモリのどこかに置かれているはずなので、FDT のマジック
 * (0xd00dfeed, ビッグエンディアン) を目印に候補を当たる。
 * 見つからなければ 0 を返す。呼ぶ側は直書きのアドレスに退くこと。 */
#define FDT_MAGIC 0xd00dfeedU

static uint32_t be32(uint32_t v) {
    return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8)  | ((v & 0xff000000U) >> 24);
}

static uint64_t find_dtb(uint64_t hint) {
    static const uint64_t candidates[] = {
        0x40000000UL,   /* RAM 先頭。QEMU virt が Linux 起動で置く場所 */
        0x48000000UL,   /* RAM 先頭 + 128MB */
        0x44000000UL,
    };
    if (hint && be32(mmio_read32(hint)) == FDT_MAGIC) return hint;
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (be32(mmio_read32(candidates[i])) == FDT_MAGIC) return candidates[i];
    }
    return 0;
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

    {
        uint64_t dtb = find_dtb(dtb_phys);
        aarch64_uart_puts("  DTB found : ");
        if (dtb) {
            put_hex64(dtb);
        } else {
            aarch64_uart_puts("none (アドレス直書きで進む)");
        }
        aarch64_uart_puts("\n");
    }

    /* .bss が実際に 0 で埋まっているかを見る。start.S の埋め方を間違えると
     * ここから先の C が静かに壊れるので、先に確かめておく */
    {
        static uint64_t bss_probe;
        aarch64_uart_puts("  bss zero  : ");
        aarch64_uart_puts(bss_probe == 0 ? "ok" : "BAD");
        aarch64_uart_puts("\n");
    }

    aarch64_uart_puts("aarch64-boot-ok\n");

    /* ---- M1: 例外ベクタ + GIC + generic timer -------------------------- */
    aarch64_vectors_init();
    aarch64_gic_init();
    aarch64_gic_enable_irq(AARCH64_IRQ_TIMER);
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

    aarch64_wait_forever();
}

/* 例外ベクタから呼ばれる。IRQ の入口 */
void aarch64_irq_handler(void) {
    uint32_t iar = aarch64_gic_claim();
    uint32_t intid = iar & 0x3ffU;

    if (intid == AARCH64_IRQ_TIMER) {
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
