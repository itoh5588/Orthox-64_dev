/*
 * AArch64 の MMU (M2)。恒等マッピング (VA == PA) で有効化するところまで。
 *
 * riscv64 は satp にテーブルの物理アドレスを書いて Sv39 (3 段) を使った。
 * AArch64 で違うのは主に 3 点:
 *
 *   1. ベースレジスタが 2 本ある (TTBR0_EL1 = 下位側 / TTBR1_EL1 = 上位側)。
 *      ここでは カーネルが 0x40200000 = 下位側に居るので TTBR0 だけを使い、
 *      TTBR1 は TCR_EL1.EPD1 = 1 で「歩かない」ことにしてある。
 *      カーネルを上位半分へ移すのは M2b (リンク先の変更を伴う)。
 *
 *   2. descriptor に AF (Access Flag, bit 10) がある。**立て忘れると
 *      アクセスした瞬間に例外**になる。riscv64 の PTE には無いビット。
 *
 *   3. メモリ属性を MAIR_EL1 の 8 枠に登録し、descriptor は
 *      「何番の枠か」(AttrIndx) だけを持つ。riscv64 には無い仕組みで、
 *      **デバイスを Normal (キャッシュ有効) で張ると実機で壊れる**。
 *
 * 恒等マッピングにするのは安全のため。MMU を有効にした瞬間、PC も sp も
 * ベクタも「いま指しているアドレス」がそのまま有効でなければ即座に迷子になる。
 * VA == PA ならその心配が無い。
 */
#include <stdint.h>

void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);
void aarch64_uart_puthex64(uint64_t v);
void aarch64_wait_forever(void);

extern char __kernel_start[];
extern char __kernel_end[];
extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];
extern char __bss_start[];
extern char __bss_end[];

#define AARCH64_PAGE_SIZE   0x1000ULL
#define AARCH64_BLOCK_SIZE  0x200000ULL          /* 2MB。L2 の 1 エントリ */
#define AARCH64_PTES        512

/* ---- descriptor のビット -------------------------------------------------
 *
 * bit[1:0] が形を決める。ここが AArch64 の踏みやすいところ:
 *
 *   L1 / L2   0b01 = ブロック (そこで翻訳を終える) / 0b11 = 次の段のテーブル
 *   L3        **0b11 のみ有効**。0b01 は invalid 扱いになる
 *
 * つまり「テーブル」と「L3 のページ」が同じ 0b11 で、「ブロック」だけが 0b01。 */
#define PTE_VALID       (1ULL << 0)
#define PTE_TABLE       (1ULL << 1)   /* L1/L2 では次段テーブル、L3 ではページ */
#define PTE_ATTRINDX(n) (((uint64_t)(n)) << 2)
#define PTE_AP_RW_EL1   (0ULL << 6)   /* AP[2:1]=00 EL1 で読み書き、EL0 は不可 */
#define PTE_AP_RW_EL0   (1ULL << 6)   /* AP=01 EL0 からも読み書き (M3 で使う) */
#define PTE_AP_RO_EL1   (2ULL << 6)   /* AP=10 EL1 で読み取りのみ */
#define PTE_SH_INNER    (3ULL << 8)   /* Inner Shareable。Normal メモリに付ける */
#define PTE_AF          (1ULL << 10)  /* Access Flag。**必須** */
#define PTE_nG          (1ULL << 11)  /* 0 = global。カーネル領域は 0 のまま */
#define PTE_PXN         (1ULL << 53)  /* EL1 からの実行禁止 */
#define PTE_UXN         (1ULL << 54)  /* EL0 からの実行禁止 */

#define PTE_ADDR_MASK   0x0000fffffffff000ULL

/* ---- MAIR_EL1 の枠 -------------------------------------------------------
 *
 *   0 番: Device-nGnRnE (0x00)  MMIO 用。並べ替えも結合もキャッシュもしない
 *   1 番: Normal, Inner/Outer Write-Back Read/Write-Allocate (0xff)  RAM 用
 *
 * デバイスを 1 番で張ると、書き込みがキャッシュに溜まって UART に届かない、
 * 読み込みが古い値を返す、といった形で壊れる。QEMU では見逃せても実機で出る。 */
#define MAIR_ATTR_DEVICE_nGnRnE 0x00ULL
#define MAIR_ATTR_NORMAL_WB     0xffULL
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1
#define MAIR_EL1_VALUE  ((MAIR_ATTR_DEVICE_nGnRnE << (8 * MAIR_IDX_DEVICE)) | \
                         (MAIR_ATTR_NORMAL_WB     << (8 * MAIR_IDX_NORMAL)))

/* ---- TCR_EL1 -------------------------------------------------------------
 *
 * T0SZ = 25 → VA は 64 - 25 = 39bit。riscv64 の Sv39 と同じ 3 段構成になる
 * (L1 が 1GB / L2 が 2MB / L3 が 4KB を受け持つ)。
 *
 * **TG0 と TG1 で 4KB の符号が違う**。TG0 は 0b00、TG1 は 0b10。
 * ここを揃えて書くのが定番の間違い。 */
#define TCR_T0SZ(n)     ((uint64_t)(n))          /* bits 5:0  */
#define TCR_IRGN0_WBWA  (1ULL << 8)
#define TCR_ORGN0_WBWA  (1ULL << 10)
#define TCR_SH0_INNER   (3ULL << 12)
#define TCR_TG0_4K      (0ULL << 14)             /* TG0: 0b00 = 4KB */
#define TCR_T1SZ(n)     (((uint64_t)(n)) << 16)
#define TCR_EPD1        (1ULL << 23)             /* TTBR1 を歩かない */
#define TCR_TG1_4K      (2ULL << 30)             /* TG1: 0b10 = 4KB (TG0 と違う) */
#define TCR_IPS_40BIT   (2ULL << 32)             /* 物理アドレス 40bit (1TB) */

#define AARCH64_VA_BITS 39
#define TCR_EL1_VALUE   (TCR_T0SZ(64 - AARCH64_VA_BITS) | \
                         TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | \
                         TCR_TG0_4K | \
                         TCR_T1SZ(64 - AARCH64_VA_BITS) | TCR_EPD1 | TCR_TG1_4K | \
                         TCR_IPS_40BIT)

/* ---- SCTLR_EL1 -----------------------------------------------------------
 *
 * **起動時の値を読んで OR しない。** そうすると土台が起動経路で変わる:
 *
 *   QEMU virt を EL1 で起動   → QEMU のリセット値 (0x00c50838) が土台
 *   EL2 から降格して来た      → start.S が書いた最小値 (0x30d00800) が土台
 *
 * 実測で、同じカーネルが 0x00c5183d と 0x30d01805 という別の設定で走って
 * いた (SA / SA0 = スタック整列チェックが EL2 経路だけ落ちていた)。
 * **Raspberry Pi 4 は必ず EL2 起動なので、QEMU で確かめた設定と実機の設定が
 * 違う**ことになる。カーネルが自分で全ビットを決めれば、この差は消える。 */
#define SCTLR_M     (1ULL << 0)     /* MMU 有効 */
#define SCTLR_C     (1ULL << 2)     /* データキャッシュ有効 */
#define SCTLR_SA    (1ULL << 3)     /* EL1 のスタック整列チェック */
#define SCTLR_SA0   (1ULL << 4)     /* EL0 のスタック整列チェック (M3 で効く) */
#define SCTLR_I     (1ULL << 12)    /* 命令キャッシュ有効 */
#define SCTLR_RES1  0x30d00800ULL   /* 立てておくと決まっているビット */

#define SCTLR_EL1_VALUE (SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_SA | SCTLR_SA0 | SCTLR_I)

/* ---- 属性の組み合わせ (呼ぶ側はこれを使う) ------------------------------ */
#define VM_ATTR_NORMAL  (PTE_ATTRINDX(MAIR_IDX_NORMAL) | PTE_SH_INNER | PTE_AF)
#define VM_ATTR_DEVICE  (PTE_ATTRINDX(MAIR_IDX_DEVICE) | PTE_AF)

#define VM_KERNEL_TEXT  (VM_ATTR_NORMAL | PTE_AP_RO_EL1 | PTE_UXN)
#define VM_KERNEL_RO    (VM_ATTR_NORMAL | PTE_AP_RO_EL1 | PTE_UXN | PTE_PXN)
#define VM_KERNEL_RW    (VM_ATTR_NORMAL | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)
#define VM_DEVICE_RW    (VM_ATTR_DEVICE | PTE_AP_RW_EL1 | PTE_UXN | PTE_PXN)

/* ---- テーブルの置き場 ----------------------------------------------------
 *
 * M2 の時点では物理メモリ管理 (pmm) がまだ無いので、.bss に固定の枠を置く。
 * 恒等マッピングなのでここのアドレスがそのまま物理アドレスになる。
 * M3 で pmm を入れたら pmm_alloc に差し替える。 */
#define AARCH64_VM_MAX_TABLES 24

static uint64_t g_tables[AARCH64_VM_MAX_TABLES][AARCH64_PTES]
    __attribute__((aligned(4096)));
static unsigned g_tables_used;
static uint64_t g_root_pa;
static int g_vm_failed;

static uint64_t* aarch64_vm_alloc_table(void) {
    uint64_t* t;
    if (g_tables_used >= AARCH64_VM_MAX_TABLES) {
        aarch64_uart_puts("  vm: table pool exhausted\n");
        g_vm_failed = 1;
        return 0;
    }
    t = g_tables[g_tables_used++];
    for (unsigned i = 0; i < AARCH64_PTES; i++) t[i] = 0;
    return t;
}

/* VA から各段のインデックスを取り出す。level 1 = bits 38:30、
 * level 2 = 29:21、level 3 = 20:12。riscv64 の VPN と考え方は同じ */
static uint64_t aarch64_vm_index(uint64_t va, int level) {
    return (va >> (12 + (3 - level) * 9)) & (AARCH64_PTES - 1);
}

/* level の段のテーブルを辿り、無ければ作る */
static uint64_t* aarch64_vm_next_table(uint64_t* table, uint64_t va, int level) {
    uint64_t index = aarch64_vm_index(va, level);
    uint64_t entry = table[index];
    uint64_t* next;

    if (entry & PTE_VALID) {
        /* 既にブロックが張られている所を細かく割ろうとしている。
         * M2 の張り方では起きないが、黙って壊さず気づけるようにする */
        if ((entry & PTE_TABLE) == 0) {
            aarch64_uart_puts("  vm: block/table conflict at 0x");
            aarch64_uart_puthex64(va);
            aarch64_uart_puts("\n");
            g_vm_failed = 1;
            return 0;
        }
        return (uint64_t*)(uintptr_t)(entry & PTE_ADDR_MASK);
    }

    next = aarch64_vm_alloc_table();
    if (!next) return 0;
    table[index] = ((uint64_t)(uintptr_t)next & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    return next;
}

/* 4KB ページを 1 枚張る。L3 の descriptor は PTE_TABLE (0b11) が必須 */
static void aarch64_vm_map_page(uint64_t va, uint64_t pa, uint64_t attr) {
    uint64_t* l1 = (uint64_t*)(uintptr_t)g_root_pa;
    uint64_t* l2 = aarch64_vm_next_table(l1, va, 1);
    uint64_t* l3;
    if (!l2) return;
    l3 = aarch64_vm_next_table(l2, va, 2);
    if (!l3) return;
    l3[aarch64_vm_index(va, 3)] = (pa & PTE_ADDR_MASK) | attr | PTE_VALID | PTE_TABLE;
}

/* 2MB ブロックを 1 つ張る。L2 の descriptor はブロックなので PTE_TABLE を
 * 立てない (0b01)。ここを 0b11 にするとテーブルとして辿られて壊れる */
static void aarch64_vm_map_block(uint64_t va, uint64_t pa, uint64_t attr) {
    uint64_t* l1 = (uint64_t*)(uintptr_t)g_root_pa;
    uint64_t* l2 = aarch64_vm_next_table(l1, va, 1);
    if (!l2) return;
    l2[aarch64_vm_index(va, 2)] = (pa & PTE_ADDR_MASK) | attr | PTE_VALID;
}

/* 範囲を張る。2MB に揃っている所はブロックで、それ以外は 4KB ページで。
 * ブロックにするとテーブルが 1 枚で 2MB 分片づくので、512MB の RAM を
 * 4KB ページで張ると 128KB 必要なところが 0 で済む */
static void aarch64_vm_map_range(uint64_t va, uint64_t pa, uint64_t size, uint64_t attr) {
    uint64_t end = va + size;
    while (va < end && !g_vm_failed) {
        if ((va % AARCH64_BLOCK_SIZE) == 0 && (pa % AARCH64_BLOCK_SIZE) == 0 &&
            (end - va) >= AARCH64_BLOCK_SIZE) {
            aarch64_vm_map_block(va, pa, attr);
            va += AARCH64_BLOCK_SIZE;
            pa += AARCH64_BLOCK_SIZE;
        } else {
            aarch64_vm_map_page(va, pa, attr);
            va += AARCH64_PAGE_SIZE;
            pa += AARCH64_PAGE_SIZE;
        }
    }
}

static uint64_t aarch64_align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t aarch64_align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* テーブルを歩いて VA の物理アドレスを求める。**MMU が実際に何を見るかを
 * 自分で辿り直す**ので、有効化前に「思ったとおりに張れているか」を確認できる */
uint64_t aarch64_vm_translate(uint64_t va) {
    uint64_t* table = (uint64_t*)(uintptr_t)g_root_pa;
    for (int level = 1; level <= 3; level++) {
        uint64_t entry = table[aarch64_vm_index(va, level)];
        if ((entry & PTE_VALID) == 0) return 0;
        if (level == 3) return (entry & PTE_ADDR_MASK) | (va & 0xfffULL);
        if ((entry & PTE_TABLE) == 0) {
            /* ブロック。ここで翻訳が終わる */
            uint64_t block_size = (level == 1) ? (1ULL << 30) : AARCH64_BLOCK_SIZE;
            return (entry & PTE_ADDR_MASK) | (va & (block_size - 1));
        }
        table = (uint64_t*)(uintptr_t)(entry & PTE_ADDR_MASK);
    }
    return 0;
}

/* ---- QEMU virt の実測値 (kernel/aarch64/boot.c 冒頭と同じ出どころ) ------ */
#define AARCH64_RAM_BASE    0x40000000ULL
#define AARCH64_RAM_SIZE    0x20000000ULL   /* 512MB */
#define AARCH64_GIC_BASE    0x08000000ULL   /* Distributor + CPU Interface */
#define AARCH64_GIC_SIZE    0x00020000ULL
#define AARCH64_UART_BASE   0x09000000ULL
#define AARCH64_UART_SIZE   0x00001000ULL
#define AARCH64_VIRTIO_BASE 0x0a000000ULL
#define AARCH64_VIRTIO_SIZE 0x00004000ULL   /* 0x200 x 32 スロット */

static void aarch64_vm_build_tables(void) {
    uint64_t kstart = (uint64_t)(uintptr_t)__kernel_start;
    uint64_t kend   = aarch64_align_up((uint64_t)(uintptr_t)__kernel_end, AARCH64_PAGE_SIZE);
    uint64_t kblk_start = aarch64_align_down(kstart, AARCH64_BLOCK_SIZE);
    uint64_t kblk_end   = aarch64_align_up(kend, AARCH64_BLOCK_SIZE);

    uint64_t* root = aarch64_vm_alloc_table();
    if (!root) return;
    g_root_pa = (uint64_t)(uintptr_t)root;

    /* RAM のうちカーネルが載っている 2MB ブロックの手前と奥。ここは
     * 大きいブロックで一気に張る (読み書き可・実行不可) */
    if (kblk_start > AARCH64_RAM_BASE) {
        aarch64_vm_map_range(AARCH64_RAM_BASE, AARCH64_RAM_BASE,
                             kblk_start - AARCH64_RAM_BASE, VM_KERNEL_RW);
    }
    if (kblk_end < AARCH64_RAM_BASE + AARCH64_RAM_SIZE) {
        aarch64_vm_map_range(kblk_end, kblk_end,
                             AARCH64_RAM_BASE + AARCH64_RAM_SIZE - kblk_end,
                             VM_KERNEL_RW);
    }

    /* カーネルが載っているブロックだけは 4KB ページで、区画ごとに権限を分ける。
     * .text を書き込み可のままにしない / .data を実行可のままにしない。
     * riscv64 の vm_init と同じ考え方 */
    aarch64_vm_map_range((uint64_t)(uintptr_t)__text_start,
                         (uint64_t)(uintptr_t)__text_start,
                         (uint64_t)(uintptr_t)__text_end - (uint64_t)(uintptr_t)__text_start,
                         VM_KERNEL_TEXT);
    aarch64_vm_map_range((uint64_t)(uintptr_t)__rodata_start,
                         (uint64_t)(uintptr_t)__rodata_start,
                         (uint64_t)(uintptr_t)__rodata_end - (uint64_t)(uintptr_t)__rodata_start,
                         VM_KERNEL_RO);
    aarch64_vm_map_range((uint64_t)(uintptr_t)__data_start,
                         (uint64_t)(uintptr_t)__data_start,
                         (uint64_t)(uintptr_t)__data_end - (uint64_t)(uintptr_t)__data_start,
                         VM_KERNEL_RW);
    aarch64_vm_map_range((uint64_t)(uintptr_t)__bss_start,
                         (uint64_t)(uintptr_t)__bss_start,
                         (uint64_t)(uintptr_t)__bss_end - (uint64_t)(uintptr_t)__bss_start,
                         VM_KERNEL_RW);
    /* 同じ 2MB ブロックの残り。空けたままだと後で使えないので張っておく */
    if (kend < kblk_end) {
        aarch64_vm_map_range(kend, kend, kblk_end - kend, VM_KERNEL_RW);
    }

    /* MMIO。**Device 属性で張ること。** Normal で張ると UART への書き込みが
     * キャッシュに溜まって出てこない、GIC の読みが古い値を返す、という形で
     * 壊れる (QEMU では見逃せても実機で出る) */
    aarch64_vm_map_range(AARCH64_GIC_BASE, AARCH64_GIC_BASE,
                         AARCH64_GIC_SIZE, VM_DEVICE_RW);
    aarch64_vm_map_range(AARCH64_UART_BASE, AARCH64_UART_BASE,
                         AARCH64_UART_SIZE, VM_DEVICE_RW);
    aarch64_vm_map_range(AARCH64_VIRTIO_BASE, AARCH64_VIRTIO_BASE,
                         AARCH64_VIRTIO_SIZE, VM_DEVICE_RW);
}

/* MMU を入れる。順序が全て:
 *
 *   MAIR / TCR / TTBR を書く → isb で見えるようにする
 *   → TLB を捨てる (MMU off の間の情報が残っている)
 *   → SCTLR の M を立てる → isb
 *
 * 割り込みは閉じておく。有効化の最中に例外が入っても恒等マッピングなら
 * 破綻しないが、切り分けを楽にするために閉じる */
static void aarch64_mmu_enable(void) {
    __asm__ volatile("dsb ishst");   /* 組んだテーブルをテーブルウォーカに見せる */

    __asm__ volatile("msr mair_el1, %0" :: "r"((uint64_t)MAIR_EL1_VALUE));
    __asm__ volatile("msr tcr_el1,  %0" :: "r"((uint64_t)TCR_EL1_VALUE));
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(g_root_pa));
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"(0ULL));
    __asm__ volatile("isb");

    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb nsh");
    __asm__ volatile("isb");

    /* 読んで OR せず、決めた値をそのまま書く (上の SCTLR_EL1_VALUE を参照) */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"((uint64_t)SCTLR_EL1_VALUE));
    __asm__ volatile("isb");
}

static uint64_t aarch64_read_sctlr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

/* ---- 未マップの VA を読む探針 --------------------------------------------
 *
 * **恒等マッピングだけでは「MMU が効いている」証拠にならない。** VA == PA
 * なので、MMU が入っていなくても出力はまったく同じになるからだ。
 *
 * そこで、どこにも張っていない VA を読んで translation fault が上がることを
 * 確かめる。上がれば「翻訳が実際に行われている」ことになる。
 * 例外ハンドラ側 (boot.c の aarch64_sync_exception) がこのフラグを見て、
 * 命令 1 つぶん進めて戻してくれる。 */
#define AARCH64_VM_PROBE_VA 0x20000000ULL   /* RAM の手前・デバイスの奥。未マップ */

volatile int g_aarch64_vm_expect_fault;
volatile uint64_t g_aarch64_vm_fault_esr;

static void aarch64_vm_fault_probe(void) {
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)AARCH64_VM_PROBE_VA;
    uint64_t esr, ec, dfsc;

    /* 先に「張られていない」ことをテーブル側で確かめる。ここが 0 でなければ
     * 探針の前提が崩れているので、fault が上がらなくても当然になる */
    if (aarch64_vm_translate(AARCH64_VM_PROBE_VA) != 0) {
        aarch64_uart_puts("  mmu probe : SKIP (探針アドレスが張られている)\n");
        return;
    }

    g_aarch64_vm_fault_esr = 0;
    g_aarch64_vm_expect_fault = 1;
    (void)*p;                       /* ここで data abort になるはず */
    g_aarch64_vm_expect_fault = 0;

    esr = g_aarch64_vm_fault_esr;
    if (esr == 0) {
        /* 読めてしまった = MMU が翻訳していない (SCTLR.M が立っていない等) */
        aarch64_uart_puts("  mmu probe : BAD (未マップの VA が読めた)\n");
        return;
    }

    ec   = (esr >> 26) & 0x3fULL;   /* Exception Class */
    dfsc = esr & 0x3fULL;           /* Data Fault Status Code */

    aarch64_uart_puts("  mmu probe : ESR=");
    aarch64_uart_puthex64(esr);
    /* EC=0x25 は「同じ EL からのデータアボート」。
     * DFSC が 0b0001xx なら translation fault (下位 2bit が段) */
    if (ec == 0x25ULL && (dfsc >> 2) == 0x1ULL) {
        aarch64_uart_puts(" (translation fault, level ");
        aarch64_uart_putchar((char)('0' + (int)(dfsc & 3ULL)));
        aarch64_uart_puts(") ok\n");
    } else {
        aarch64_uart_puts(" BAD (translation fault ではない)\n");
    }
}

void aarch64_vm_init(void) {
    uint64_t kstart = (uint64_t)(uintptr_t)__kernel_start;

    aarch64_uart_puts("--- M2: MMU (identity, 4KB granule, VA 39bit) ---\n");

    /* カーネルの先頭が 2MB 境界に無いと、カーネルの載るブロックの張り分けが
     * 成り立たない。リンカスクリプトを動かしたときに黙って壊れないよう見る */
    if (kstart % AARCH64_BLOCK_SIZE) {
        aarch64_uart_puts("  kernel start が 2MB 境界に無い\n");
        aarch64_uart_puts("aarch64-mmu-BAD\n");
        return;
    }

    aarch64_vm_build_tables();
    if (g_vm_failed) {
        aarch64_uart_puts("aarch64-mmu-BAD (テーブル構築に失敗)\n");
        return;
    }

    aarch64_uart_puts("  tables    : ");
    aarch64_uart_puthex64(g_tables_used);
    aarch64_uart_puts(" / ");
    aarch64_uart_puthex64(AARCH64_VM_MAX_TABLES);
    aarch64_uart_puts("\n");

    /* 有効化の前に、自分でテーブルを歩いて要るものが張れているかを見る。
     * ここで 0 が出るものがあれば、MMU を入れた瞬間に沈黙する */
    aarch64_uart_puts("  text ->pa : ");
    aarch64_uart_puthex64(aarch64_vm_translate((uint64_t)(uintptr_t)__text_start));
    aarch64_uart_puts("\n  uart ->pa : ");
    aarch64_uart_puthex64(aarch64_vm_translate(AARCH64_UART_BASE));
    aarch64_uart_puts("\n  gic  ->pa : ");
    aarch64_uart_puthex64(aarch64_vm_translate(AARCH64_GIC_BASE));
    aarch64_uart_puts("\n");
    {
        uint64_t sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        aarch64_uart_puts("  sp   ->pa : ");
        aarch64_uart_puthex64(aarch64_vm_translate(sp));
        aarch64_uart_puts("\n");
    }

    if (aarch64_vm_translate((uint64_t)(uintptr_t)__text_start) == 0 ||
        aarch64_vm_translate(AARCH64_UART_BASE) == 0) {
        aarch64_uart_puts("aarch64-mmu-BAD (有効化前の確認で 0 が出た)\n");
        return;
    }

    __asm__ volatile("msr daifset, #2");    /* 有効化の間は割り込みを閉じる */
    aarch64_mmu_enable();
    __asm__ volatile("msr daifclr, #2");

    /* ここに文字が出ること自体が「MMU on のまま UART に届いている」証拠 */
    aarch64_uart_puts("  SCTLR_EL1 : ");
    aarch64_uart_puthex64(aarch64_read_sctlr());
    aarch64_uart_puts("\n");

    if ((aarch64_read_sctlr() & SCTLR_M) == 0) {
        aarch64_uart_puts("aarch64-mmu-BAD (SCTLR.M が立っていない)\n");
        return;
    }

    aarch64_vm_fault_probe();
}

uint64_t aarch64_vm_root_pa(void) {
    return g_root_pa;
}
