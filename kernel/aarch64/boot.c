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
#include "aarch64/fb.h"
#include "aarch64/task.h"
#include "aarch64/vm.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"

/* PL011 UART。QEMU は初期化なしでも DR に書けば出るが、実機と手順を
 * 揃えておく (M1 以降で割り込み受信を足すときに効く)
 *
 * **DTB を読む前に UART が要る**という循環がある (解析結果を表示したい)。
 * 既定値で出し始め、DTB が別の場所を指していたら差し替える。
 * Pi 4 の PL011 は 0xFE201000 なので、**実機では差し替え前の出力が
 * 捨てられる**。段取り 3 で既定値の決め方が要る。 */
static uint64_t g_uart_base = AARCH64_EARLY_UART;

#define PL011_DR_OFF    0x00
#define PL011_FR_OFF    0x18
#define PL011_FR_TXFF   (1U << 5)   /* 送信 FIFO が満杯 */
#define PL011_FR_RXFE   (1U << 4)   /* 受信 FIFO が空 */
/* 受信割り込み (P3)。IMSC で有効化し、ICR で確認応答する。
 * **RT (受信タイムアウト) も要る。** RX だけだと FIFO の閾値に達しない
 * 半端な入力が割り込みにならず、対話シェルで 1 文字打っても届かない */
#define PL011_IMSC_OFF  0x38
#define PL011_ICR_OFF   0x44
#define PL011_INT_RX    (1U << 4)
#define PL011_INT_RT    (1U << 6)

static inline void mmio_write32(uint64_t addr, uint32_t v) {
    *(volatile uint32_t*)addr = v;
}

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

void aarch64_uart_set_base(uint64_t base) {
    if (base) g_uart_base = base;
}

/* 出力の 1 単位。**並行に走るタスクに行の途中を割られないようにする。**
 * タスク層より前 (aarch64_task_init の前) から呼ばれるが、そのときは
 * スケジューラが止まっているので数えるだけで済む */
void aarch64_console_begin(void) { aarch64_preempt_disable(); }
void aarch64_console_end(void)   { aarch64_preempt_enable(); }

/* 1 文字は MMIO の書き込み 1 回なので、これ自体は割れようがない。
 *
 * **画面にも同じものを流す。** シリアルの代わりではなく増設 —
 * 画面が無い機械 (QEMU virt) では aarch64_fbcon_putc が何もしないので、
 * ここに条件は書かない */
void aarch64_uart_putchar(char c) {
    while (mmio_read32(g_uart_base + PL011_FR_OFF) & PL011_FR_TXFF) {
        /* 送信 FIFO が空くまで待つ */
    }
    mmio_write32(g_uart_base + PL011_DR_OFF, (uint32_t)(unsigned char)c);
    aarch64_fbcon_putc(c);
}

/* 1 文字だけ取る。無ければ -1 (P3)。**待たない** —
 * 待つかどうかを決めるのは呼び出し側 (kernel/aarch64/console.c) */
int aarch64_uart_getchar_nonblock(void) {
    if (mmio_read32(g_uart_base + PL011_FR_OFF) & PL011_FR_RXFE) return -1;
    return (int)(mmio_read32(g_uart_base + PL011_DR_OFF) & 0xFFU);
}

/* 受信割り込みを開ける (P3)。GIC 側の有効化とは別で、**両方要る** */
void aarch64_uart_enable_rx_irq(void) {
    mmio_write32(g_uart_base + PL011_ICR_OFF, PL011_INT_RX | PL011_INT_RT);
    mmio_write32(g_uart_base + PL011_IMSC_OFF, PL011_INT_RX | PL011_INT_RT);
}

/* 受信割り込みの確認応答。**これを忘れると同じ割り込みが上がり続ける** */
void aarch64_uart_clear_rx_irq(void) {
    mmio_write32(g_uart_base + PL011_ICR_OFF, PL011_INT_RX | PL011_INT_RT);
}

/* **文字列 1 本は割れない。** マルチバイト文字の途中で切り替わると
 * 文字そのものが壊れるので、ここは囲んでおく価値がある */
void aarch64_uart_puts(const char* s) {
    if (!s) return;
    aarch64_console_begin();
    while (*s) {
        if (*s == '\n') aarch64_uart_putchar('\r');
        aarch64_uart_putchar(*s);
        s++;
    }
    aarch64_console_end();
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

/* 10 進。**解像度やピッチは 16 桁の 16 進で出しても読めない**ので、
 * 人が見る数はこちらで出す (emmc2.c が同じ理由で putdec を持っている) */
static void put_dec(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) { aarch64_uart_putchar('0'); return; }
    while (v > 0 && i < (int)sizeof(buf)) { buf[i++] = (char)('0' + (int)(v % 10U)); v /= 10U; }
    while (i > 0) aarch64_uart_putchar(buf[--i]);
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
void aarch64_pmm_init(void);
uint64_t aarch64_pmm_total(void);
uint64_t aarch64_pmm_used(void);
uint64_t aarch64_pmm_meta_pages(void);
int aarch64_pmm_meta_fault(void);
uint64_t aarch64_vm_translate(uint64_t va);
void aarch64_vm_fault_probe(void);
void aarch64_vm_drop_identity(void);
uint64_t aarch64_read_sctlr(void);
uint64_t aarch64_vm_kernel_root_pa(void);
uint64_t aarch64_vm_user_root_pa(void);
void aarch64_gic_set_base(uint64_t gicd, uint64_t gicc);
int aarch64_user_run(void);
int aarch64_virtio_blk_init(void);
int aarch64_virtio_blk_read(uint64_t lba, void* buf, uint32_t sectors);
int aarch64_virtio_blk_write(uint64_t lba, const void* buf, uint32_t sectors);
uint64_t aarch64_virtio_blk_capacity(void);
uint64_t aarch64_virtio_blk_base_pa(void);
void aarch64_virtio_blk_selftest(void);
int aarch64_shared_layer_selftest(void);
int aarch64_shared_task_selftest(void);
int aarch64_fs_selftest(void);
int aarch64_first_user_task(void);
uint32_t aarch64_virtio_blk_intid(void);
void aarch64_virtio_blk_irq(void);
uint32_t aarch64_uart_intid(void);
void aarch64_console_rx_irq(void);
void aarch64_console_input_init(void);
void pci_init(void);
void aarch64_pci_dump(void);
int aarch64_pcie_brcm_probe(void);
int aarch64_pcie_brcm_init(void);
int aarch64_pcie_brcm_scan(void);
int aarch64_fb_init_pci(uint32_t w, uint32_t h);
int aarch64_pci_ready(void);
void usb_init(void);
int usb_is_ready(void);
int usb_hid_keyboard_init(void);
int usb_hid_keyboard_poll(uint8_t report[8]);
uint64_t arch_time_now_ms(void);
int aarch64_kbd_get_event(struct key_event* ev);

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

/* リンカスクリプトが置く印。**MMU の前に読むと物理アドレスが返る** */
extern char __kernel_start[];

/* start.S が「降ろす前の CurrentEL」を入れる。**C から読む CurrentEL は
 * 降格後の値なので、どの EL で飛んできたかはこちらでしか分からない** */
uint64_t aarch64_entry_el;

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
    /* virtio の番地は既定値で埋めない。**「DTB に無い」と「まだ読んでいない」
     * を 0 で見分ける**ため。読めなかったときの退き先は走査の後で入れる */
    g_boot_info.first_virtio_mmio_base = 0;
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

    /* **RAM が分からないときは、自分がどこに載っているかから導く。**
     * 機械ごとの直書きに退くと、別の機械では存在しない番地を張ってしまう
     * (include/aarch64/boot.h の注記)。
     *
     * ここはまだ MMU の前なので、リンカシンボルを C から取ると
     * **物理アドレスが返る** (adrp が PC 相対で、PC が物理のため。
     * vm.c の aarch64_vm_ptr_pa が同じ前提に立っている) */
    if (g_boot_info.memory_size == 0) {
        uint64_t kernel_pa = (uint64_t)(uintptr_t)__kernel_start;
        g_boot_info.memory_base = kernel_pa & ~(AARCH64_FALLBACK_RAM_ALIGN - 1);
        g_boot_info.memory_size = AARCH64_FALLBACK_RAM_SIZE;
    }
    if (g_boot_info.virtio_mmio_stride == 0) {
        g_boot_info.virtio_mmio_stride = AARCH64_QEMU_VIRT_VIRTIO_STRIDE;
    }
    /* **DTB が読めたのに virtio が 1 つも無いなら、その機械には無い。**
     * virt の既定値を残すと Pi 4 で 0x0a000000 を Device で張ろうとし、
     * RAM のブロックと衝突してテーブル構築ごと失敗する。
     * DTB そのものが読めなかったときだけ、従来どおり virt の値に退く */
    if (!(g_boot_info.flags & AARCH64_BOOT_FLAG_DTB_VALID)) {
        g_boot_info.first_virtio_mmio_base = AARCH64_QEMU_VIRT_VIRTIO_BASE;
    }

#ifdef AARCH64_EMMC2_BASE_OVERRIDE
    /* **検証専用の差し込み口。既定では定義しない。**
     *
     * QEMU の raspi4b は SD カードを **旧 sdhci (0xFE300000) に繋いでいて、
     * EMMC2 (0xFE340000) は空のまま**にしている
     * (hw/arm/bcm2838_peripherals.c が GPIO の sdbus-sdhci を
     *  s_base->sdhci に結んでいる)。**実機の Pi 4 とは配線が違う。**
     *
     * 両方とも QEMU では同じ generic-sdhci なので、番地だけ差し替えれば
     * ドライバの中身 (初期化手順 / PIO 転送 / LBA の単位) は確かめられる。
     * **実機向けのビルドでは定義しないこと** — 実機の 0xFE300000 は
     * WiFi の SDIO で、SD カードではない */
    g_boot_info.emmc2_base = AARCH64_EMMC2_BASE_OVERRIDE;
    g_boot_info.emmc2_size = 0x100;
    g_boot_info.flags &= ~AARCH64_BOOT_FLAG_EMMC2_FROM_DTB;
#endif

    /* UART が別の場所だと分かったらここで乗り換える。
     *
     * **他機の DTB を読ませて検証するときは乗り換えると何も見えなくなる。**
     * 例: QEMU virt に Pi 4 の DTB を -dtb で食わせると、ここで
     * 0xfe201000 へ移ってしまい、以降の表示が届かない。
     * ORTHOX_DTB_KEEP_UART を渡すと乗り換えを止めて、
     * **解釈結果だけを手元の UART で読める。** */
#ifndef ORTHOX_DTB_KEEP_UART
    aarch64_uart_set_base(g_boot_info.uart_base);
#endif
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

    /* **レンジが 2 つ以上なら中身を出す。**
     * Raspberry Pi 4 (4GB) は RAM が割れていて、上の size は穴を含む全体。
     * **配れるのはここに出るぶんだけ**なので、食い違いが見えるようにする */
    if (b->mem_range_count > 1) {
        for (uint32_t i = 0; i < b->mem_range_count; i++) {
            aarch64_uart_puts("  mem range : ");
            put_hex64(b->mem_range_base[i]);
            aarch64_uart_puts(" + ");
            put_hex64(b->mem_range_size[i]);
            aarch64_uart_puts("\n");
        }
    }

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

    /* **Pi 4 の SD カード。** QEMU virt には無い。
     * **「この機械には無い」と「取り損ねた」を混ぜない。** 無いときに
     * (既定値) と出すと、退いた値を使っているように見える (実際には
     * 退き先が無い) し、スモークの「全部 DTB 由来か」の判定にも当たる */
    aarch64_uart_puts("  emmc2     : ");
    if (b->emmc2_base == 0) {
        aarch64_uart_puts("この機械には無い\n");
    } else {
        put_hex64(b->emmc2_base);
        aarch64_uart_puts(" size ");
        put_hex64(b->emmc2_size);
        aarch64_uart_puts(" irq ");
        put_hex64(b->emmc2_intid);
        put_src(AARCH64_BOOT_FLAG_EMMC2_FROM_DTB);
    }

    aarch64_uart_puts("  timer irq : ");
    put_hex64(b->timer_intid);
    put_src(AARCH64_BOOT_FLAG_TIMER_FROM_DTB);

    /* PL011 の受信割り込み (P3)。**QEMU virt では既定値と同じ 33 になるので、
     * 値だけ見ても DTB を読めた証拠にならない。** どこから来たかを出す */
    aarch64_uart_puts("  uart irq  : ");
    put_hex64(aarch64_uart_intid());
    put_src(AARCH64_BOOT_FLAG_UART_IRQ_FROM_DTB);

    /* **スロット i の INTID = base + i が成り立つと確かめられたときだけ
     * 有効。** 確かめずに決め打ちすると、別のデバイスの割り込みを待つ */
    aarch64_uart_puts("  virtio irq: ");
    put_hex64(b->virtio_mmio_irq_base);
    aarch64_uart_puts((b->flags & AARCH64_BOOT_FLAG_VIRTIO_IRQ_OK)
                      ? "  (dtb、base + スロット番号で確認済み)\n"
                      : "  (確かめられない。ポーリングに退く)\n");

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

    /* **入口の EL と、いまの EL の両方を出す。**
     * start.S が EL3 -> EL2 -> EL1 と降ろすので、ここで読める CurrentEL は
     * 常に EL1 になる。それだけを見て「この機械は EL1 で来る」と読むと
     * 間違える (raspi4b を EL3 起動と読み違えた)。
     * 入口の EL は start.S が降ろす前に控えたもの */
    aarch64_uart_puts("  CurrentEL : EL");
    aarch64_uart_putchar((char)('0' + (int)(read_current_el() & 3U)));
    aarch64_uart_puts("  (入口 EL");
    aarch64_uart_putchar((char)('0' + (int)(aarch64_entry_el & 3U)));
    aarch64_uart_puts(")\n");


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

    /* **DTB の後、MMU の前。** pmm は DTB が言う RAM の広さを要るし、
     * vm はページテーブルを pmm から取る */
    /* **共有層の名前のほうを呼ぶ (M3c-2a)。** 中で aarch64_pmm_init を
     * 呼んだうえで g_hhdm_offset を入れる。aarch64_pmm_init を直接
     * 呼ぶと、共有層が物理 → VA の変換に使う値が 0 のままになる */
    pmm_init();
    aarch64_uart_puts("  pmm       : ");
    put_hex64(aarch64_pmm_total());
    aarch64_uart_puts(" ページ (使用 ");
    put_hex64(aarch64_pmm_used());
    aarch64_uart_puts(")\n");

    /* **管理情報 (ビットマップ + refcount) は RAM から切り出している。**
     * 静的配列をやめたので、「何ページ取れたか」は数字で見えないと
     * 確かめようがない。0 ページなら切り出しに失敗している */
    aarch64_uart_puts("  pmm meta  : ");
    put_hex64(aarch64_pmm_meta_pages());
    aarch64_uart_puts(" ページ");
    if (aarch64_pmm_meta_fault()) {
        aarch64_uart_puts("  BAD (実在しない RAM の上に置こうとした)");
    } else if (aarch64_pmm_meta_pages() == 0) {
        aarch64_uart_puts("  BAD (切り出せていない)");
    } else {
        aarch64_uart_puts("  ok (RAM から切り出した)");
    }
    aarch64_uart_puts("\n");

    /* PCIe。**QEMU virt にはあるが raspi4b には無い。**
     * 実機の USB は PCIe の先の VL805 なので、ここが 0 のままだと
     * 「この機械では USB を探しに行けない」ことを意味する */
    {
        const aarch64_boot_info_t* b2 = aarch64_boot_info();
        aarch64_uart_puts("  pcie ecam : ");
        if (b2->pcie_ecam_base) {
            put_hex64(b2->pcie_ecam_base);
            aarch64_uart_puts(" size ");
            put_hex64(b2->pcie_ecam_size);
            aarch64_uart_puts("  (dtb)\n");
            aarch64_uart_puts("  pcie mmio : ");
            put_hex64(b2->pcie_mmio_base);
            aarch64_uart_puts(" size ");
            put_hex64(b2->pcie_mmio_size);
            aarch64_uart_puts(b2->pcie_mmio_base ? "  (BAR の置き場)\n"
                                                 : "  BAD (32bit 窓が無い)\n");
        } else {
            aarch64_uart_puts("無し (この機械には ECAM の PCIe が無い)\n");
        }
    }

    /* **PCI の走査は MMU の後にしたい**が、BAR を配るのは早いほうがよい。
     * ECAM は恒等マッピングにも HHDM にも無いので、**MMU を入れてから**
     * (aarch64_vm_init の後で) 呼ぶ。ここではまだ呼ばない */

    /* ---- 画面 (Raspberry Pi のみ) ---------------------------------------
     *
     * **MMU より前。** mailbox のバッファにキャッシュ管理が要らないのと、
     * 返ってくる番地が GPU の予約領域 (pmm の管理外) なので、
     * ページを確保せずに済むため。詳しくは kernel/aarch64/fb.c の冒頭 */
    {
        const aarch64_fb_info_t* fb;
        aarch64_fb_init(0, 0);
        fb = aarch64_fb_info();
        aarch64_uart_puts("  fb        : ");
        if (fb->base != 0) {
            put_hex64(fb->base);
            aarch64_uart_puts(" ");
            put_dec(fb->width);
            aarch64_uart_puts("x");
            put_dec(fb->height);
            aarch64_uart_puts("x");
            put_dec(fb->depth);
            aarch64_uart_puts(" pitch ");
            put_dec(fb->pitch);
            aarch64_uart_puts("  ok\n");
            /* **画面に出す前にログへ出す。** 絵が出ないとき、番地を取れて
             * いないのか描けていないのかを切り分けられるようにする。
             *
             * テストパターンを一瞬出してからコンソールで塗り潰す。
             * **絵が出るかどうかと、字が出るかどうかは別の話** — 文字が
             * 化けているときに「そもそも描けているのか」を見分けられる */
            aarch64_fb_test_pattern();
            aarch64_uart_puts("  fb pattern: 描いた\n");
            aarch64_fbcon_init();
            aarch64_uart_puts("  fb console: ");
            if (aarch64_fbcon_ready()) {
                put_dec(aarch64_fbcon_cols());
                aarch64_uart_puts("桁 x ");
                put_dec(aarch64_fbcon_rows());
                aarch64_uart_puts("行  ok (ここから先は HDMI にも出る)\n");
            } else {
                aarch64_uart_puts("BAD (立ち上げられない)\n");
            }
        } else if (fb->fail == AARCH64_FB_FAIL_NO_MBOX) {
            aarch64_uart_puts("無し (mailbox が無い機械)\n");
        } else {
            aarch64_uart_puts("BAD (fail=");
            put_dec(fb->fail);
            aarch64_uart_puts(" 1=mbox無 2=時間切れ 3=VC拒否 4=返事が変)\n");
        }
    }

    /* **0 のまま進んでいないことを出させる。** 値が正しいことと、
     * 設定されたことは別 (日報2026-08-09 追-7)。共有層はここを使って
     * 物理 → VA を作るので、0 のままだと恒等を外した瞬間に落ちる */
    aarch64_uart_puts("  hhdm      : ");
    put_hex64(g_hhdm_offset);
    aarch64_uart_puts(g_hhdm_offset == AARCH64_KERNEL_VA_OFFSET
                      ? "  ok\n" : "  BAD (共有層の物理→VA が壊れる)\n");

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

    /* ---- M2 / M3b: MMU を入れて上位 VA へ移る --------------------------
     *
     * **aarch64_vm_init は戻ってこない。** MMU を入れて上位 VA へ飛び、
     * 下の aarch64_boot_continue から続きが始まる */
    aarch64_vm_init();

    /* 失敗したときだけここに来る */
    aarch64_uart_puts("aarch64-mmu-BAD (上位 VA へ移れなかった)\n");
    aarch64_wait_forever();
}

/* ---- ここから上位 VA (TTBR1) で走る -------------------------------------
 *
 * スタックも飛び先も上位 VA。**ここから物理アドレスを直に触ってはいけない。**
 * DTB 由来のアドレスは必ず aarch64_phys_to_virt を通すこと。 */
void aarch64_boot_continue(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t sp;

    /* まず UART を上位 VA に乗り換える。**恒等マッピングを外す前に。**
     * 順序を逆にすると、次の 1 文字を出そうとした瞬間に落ちる */
    aarch64_uart_set_base(aarch64_phys_to_virt(b->uart_base));
    aarch64_gic_set_base(aarch64_phys_to_virt(b->gicd_base),
                         aarch64_phys_to_virt(b->gicc_base));

    /* 例外ベクタも上位 VA に張り替える */
    aarch64_vectors_init();

    __asm__ volatile("mov %0, sp" : "=r"(sp));
    aarch64_uart_puts("  high VA   : pc=");
    aarch64_uart_puthex64((uint64_t)(uintptr_t)aarch64_boot_continue);
    aarch64_uart_puts(" sp=");
    put_hex64(sp);
    aarch64_uart_puts("\n  vbar/uart : ");
    put_hex64((uint64_t)(uintptr_t)aarch64_vectors);
    aarch64_uart_puts(" / ");
    put_hex64(aarch64_phys_to_virt(b->uart_base));
    aarch64_uart_puts("\n");

    /* **恒等マッピングを外す。** ここから TTBR0 はユーザー専用。
     * この後も文字が出れば、カーネルが TTBR1 だけで走っている証拠になる */
    aarch64_vm_drop_identity();
    aarch64_uart_puts("  ttbr0     : ");
    put_hex64(aarch64_vm_user_root_pa());
    aarch64_uart_puts("  (恒等を外した。カーネルは TTBR1 だけで走っている)\n");

    aarch64_uart_puts("  SCTLR_EL1 : ");
    put_hex64(aarch64_read_sctlr());
    aarch64_uart_puts("\n");

    aarch64_vm_fault_probe();

    __asm__ volatile("msr daifclr, #2");   /* 割り込みを開け直す */

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

    /* ---- PCI (MMU の後) ---------------------------------------------------
     *
     * **ECAM は恒等マッピングに無い。** MMU を入れて TTBR1 が効いてから
     * でないと設定空間を読めない */
    pci_init();
    aarch64_pci_dump();

    /* **画面がまだ無ければ PCI の表示装置を探す。**
     * QEMU virt には mailbox が無いので、こちらでしか画面が取れない。
     * **キーボードと画面を同時に試せる唯一の機械** */
    if (aarch64_fb_info()->base == 0 && aarch64_pci_ready()) {
        if (aarch64_fb_init_pci(0, 0) == 0) {
            aarch64_fbcon_init();
            aarch64_uart_puts("  fb console: ");
            if (aarch64_fbcon_ready()) {
                put_dec(aarch64_fbcon_cols());
                aarch64_uart_puts("桁 x ");
                put_dec(aarch64_fbcon_rows());
                aarch64_uart_puts("行  ok (PCI の画面)\n");
            } else {
                aarch64_uart_puts("BAD\n");
            }
        }
    }

    /* Raspberry Pi 4 の PCIe。**読むだけの探針** — 実機で何が見えているかを
     * 確かめる段階。QEMU の raspi4b は PCIe を持っていないので何も出ない */
    /* **立ち上げが先、探針は後。**どちらも既定では何もしない
     * (AARCH64_PCIE_BRCM_INIT / _PROBE で有効化)。
     *
     * 立ち上げが成功したら、その先に VL805 (xHCI) がいるので、
     * **PCI の走査をやり直す**必要がある */
    if (aarch64_pcie_brcm_init() == 0) {
        aarch64_uart_puts("  pcie brcm : 立ち上がった。下流を探す\n");
        aarch64_pcie_brcm_scan();
    }
    aarch64_pcie_brcm_probe();

    /* xHCI。**共有層の kernel/usb.c をそのまま使う** — リングもスロットも
     * TRB もアーキに依らない。**QEMU virt でしか動かない**  (raspi4b は
     * PCIe を持っていない)。ここが通れば USB キーボードへの道が開く */
    if (aarch64_pci_ready()) {
        usb_init();
        aarch64_uart_puts("  usb       : ");
        aarch64_uart_puts(usb_is_ready() ? "ok\n" : "見つからない / 初期化できない\n");
        if (usb_is_ready()) {
            int hk = usb_hid_keyboard_init();
            aarch64_uart_puts("  usb kbd   : ");
            if (hk == 0) {
                aarch64_uart_puts("ok (boot protocol)\n");
                /* **押されていないことを 1 回読んで確かめる。**
                 * 「初期化できた」と「レポートが取れる」は別 */
                {
                    uint8_t rep[8];
                    int r = usb_hid_keyboard_poll(rep);
                    aarch64_uart_puts("  usb kbd rd: ");
                    if (r == 0) {
                        aarch64_uart_puts("レポートが取れた mod=");
                        put_dec(rep[0]);
                        aarch64_uart_puts(" key=");
                        put_dec(rep[2]);
                        aarch64_uart_puts("\n");
                    } else if (r == 1) {
                        aarch64_uart_puts("まだ何も来ていない (押されていない)\n");
                    } else {
                        aarch64_uart_puts("BAD\n");
                    }
                }
                /* **通常は探針を出さない。** ORTH_SYS_GET_KEY_EVENT が
                 * 呼ばれたときにポーリングする (kernel/aarch64/kbd.c)。
                 *
                 * スモーク用にだけ、変換した結果を出す口を残す:
                 *   make ... AARCH64_USB_KBD_PROBE=1
                 * **押した / 離した と scancode / ascii まで見ないと、
                 * 変換と差分が正しいか確かめられない** */
#ifdef AARCH64_USB_KBD_PROBE
                {
                    struct key_event ev;
                    uint64_t t0 = arch_time_now_ms();
                    int seen = 0;
                    aarch64_uart_puts("usb-kbd-probe-start\n");
                    while (arch_time_now_ms() - t0 < 12000 && seen < 24) {
                        if (aarch64_kbd_get_event(&ev)) {
                            aarch64_uart_puts("  [kbd] ");
                            aarch64_uart_puts(ev.pressed ? "down" : "up  ");
                            aarch64_uart_puts(" sc=");
                            put_dec(ev.scancode);
                            aarch64_uart_puts(" ascii=");
                            put_dec(ev.ascii);
                            aarch64_uart_puts("\n");
                            seen++;
                        }
                    }
                    aarch64_uart_puts("usb-kbd-probe-done count=");
                    put_dec((uint64_t)seen);
                    aarch64_uart_puts("\n");
                }
#endif
            } else {
                aarch64_uart_puts("無し / 初期化できない\n");
            }
        }
    }

    /* ---- 画面が MMU の後も届くか ----------------------------------------
     *
     * **ここが本番。**テストパターンは MMU の前 (物理番地) で描いており、
     * それだけでは「上位 VA から書ける」ことの証拠にならない。
     * コンソールも DOOM も MMU の後から書くので、ここで確かめておく。
     *
     * **触る前にテーブルを見る。**いきなり書いて data abort になると、
     * 「写像が無い」のか「番地が違う」のかが分からないまま落ちる */
    {
        const aarch64_fb_info_t* fb = aarch64_fb_info();
        if (fb->base != 0) {
            aarch64_uart_puts("  fb va     : ");
            put_hex64(AARCH64_FB_VA_BASE);
            if (aarch64_vm_translate(AARCH64_FB_VA_BASE) == fb->base) {
                aarch64_uart_puts("  ok (物理と一致)\n");
                aarch64_fb_mark_top(0x00ff00U);
                aarch64_uart_puts("  fb post   : 上位 VA から描いた (画面の上端が緑)\n");
            } else {
                aarch64_uart_puts("  BAD (張れていない)\n");
            }
        }
    }

    /* ---- M3c-2a: 共有層 (pmm.h / arch_vm_*) ----------------------------- */
    aarch64_shared_layer_selftest();

    /* ---- M4: virtio-mmio (virtio-blk) ---------------------------------- */
    aarch64_virtio_blk_selftest();

    /* ---- M3a/M3b/M3c: EL0 + アドレス空間 + 切り替え -------------------- */
    aarch64_user_run();

    /* ---- M3c-2b: 共有スケジューラへ乗り換える --------------------------
     * **M3c-1 の器を使う検査が全部終わってから。** 乗り換えると
     * aarch64_task_* の器は止まる */
    aarch64_shared_task_selftest();

    /* ---- M4-3: xv6fs ---------------------------------------------------
     * **task_init の後。** xv6fs は sleeplock を使うので、待てるタスクが
     * 居ないと成立しない */
    aarch64_fs_selftest();

    /* ---- P3: コンソール入力の口を開ける -------------------------------
     * **ユーザープロセスを起こす前に開ける。** exec してからでは、
     * 対話プログラムが最初の read で誰にも起こされないまま寝る */
    aarch64_console_input_init();

    /* ---- P1: ディスクの ELF を EL0 で走らせる -------------------------- */
    aarch64_first_user_task();

    aarch64_wait_forever();
}

/* 例外ベクタから呼ばれる。IRQ の入口 */
void aarch64_irq_handler(void) {
    uint32_t iar = aarch64_gic_claim();
    uint32_t intid = iar & 0x3ffU;

    if (intid == aarch64_timer_intid()) {
        aarch64_timer_on_tick();
    } else if (intid && intid == aarch64_virtio_blk_intid()) {
        aarch64_virtio_blk_irq();
    } else if (intid && intid == aarch64_uart_intid()) {
        /* コンソール入力 (P3)。リングへ移して待ち手を起こす */
        aarch64_console_rx_irq();
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
    uint64_t spsr, far, sp0;
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(spsr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, sp_el0" : "=r"(sp0));
    aarch64_uart_puts("\n*** aarch64 unexpected exception ***\n");
    aarch64_uart_puts("  vector : ");
    put_hex64(which);
    aarch64_uart_puts("\n  ESR    : ");
    put_hex64(esr);
    aarch64_uart_puts("\n  ELR    : ");
    put_hex64(elr);
    /* **ESR / ELR だけでは足りない (P3-4 で学んだ)。**
     *
     *   SPSR   : どこへ戻ろうとしていたか。M[4] が立っていれば AArch32 で、
     *            これが「壊れた ctx から eret した」決定的な証拠になる
     *   FAR    : アボートなら触ろうとしたアドレス
     *   SP_EL0 : どのタスクが死んだか (kstack の位置で当たりが付く)
     *
     * P3-4 の真因 (EL1 を割り込んだ preempt) は、vector 0xc と SPSR の
     * M[4] から辿った。この 3 つが無ければ「どこかで壊れる」までしか
     * 分からず、また仮説で当てにいくことになる */
    aarch64_uart_puts("\n  SPSR   : ");
    put_hex64(spsr);
    aarch64_uart_puts("\n  FAR    : ");
    put_hex64(far);
    aarch64_uart_puts("\n  SP_EL0 : ");
    put_hex64(sp0);
    aarch64_uart_puts("\naarch64-exception-BAD\n");
}
