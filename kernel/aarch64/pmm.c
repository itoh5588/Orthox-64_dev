/*
 * 物理メモリ管理 (M3b-2)。ビットマップで 4KB ページを配る。
 *
 * kernel/riscv64/pmm.c と同じ方式だが、あちらは共有ヘッダ (pmm.h / vmm.h /
 * spinlock.h) に繋がっている。aarch64 はまだ共有層に乗っていないので
 * 自己完結にしてある。共有層への接続は M3c。
 *
 * **管理するのは物理アドレス。** カーネルは上位 VA で走っているので、
 * 中身を触るときは aarch64_phys_to_virt を通すこと。
 *
 * 空き領域の始まりに注意:
 *
 *   カーネルイメージ (__kernel_end まで)
 *   ブートスタック   8 CPU x 64KB   ← **ここも使用中。空きにしてはいけない**
 *   ここから空き
 *
 * ブートスタックは start.S がカーネルイメージの直後に取っている
 * (.bss だと .boot から届かず、.boot に置くとイメージが 512KB 太るため)。
 * ここを空きとして配ると、いま自分が乗っているスタックを別の用途に
 * 渡すことになる。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/vm.h"

#define AARCH64_PAGE_SIZE   0x1000ULL
#define AARCH64_BOOT_CPUS   8
#define AARCH64_BOOT_STACK  65536ULL

/* 管理できるページ数の上限。4KB x 524288 = 2GB。
 *
 * **512MB だった。** DTB が 2GB と言っても pmm は 512MB で頭打ちにしていて、
 * QEMU に -m を積んでも効かなかった (1G と 2G で同じ所で停止することを実測)。
 *
 * 上げる代償は .bss:
 *    512MB  bitmap 16KB + refcount 256KB  =  272KB
 *   2048MB  bitmap 64KB + refcount 1024KB = 1088KB
 *
 * **Raspberry Pi 4 の 4GB / 8GB はこの形では賄えない** (4GB で 2176KB)。
 * 実機に行くときは、ビットマップと refcount を静的配列でなく
 * **管理対象の RAM から切り出す**必要がある。そこまでやると上限が消える。 */
#define AARCH64_PMM_MAX_PAGES 524288U

extern char __kernel_end[];

static uint8_t g_bitmap[(AARCH64_PMM_MAX_PAGES + 7U) / 8U];
static uint64_t g_base_pa;      /* 管理領域の先頭 (物理) */
static uint64_t g_pages;        /* 管理しているページ数 */
static uint64_t g_used;

static void pmm_set(uint64_t page)   { g_bitmap[page / 8U] |= (uint8_t)(1U << (page % 8U)); }
static void pmm_clear(uint64_t page) { g_bitmap[page / 8U] &= (uint8_t)~(1U << (page % 8U)); }
static int  pmm_test(uint64_t page)  { return (g_bitmap[page / 8U] >> (page % 8U)) & 1U; }

static uint64_t align_up_page(uint64_t v) {
    return (v + AARCH64_PAGE_SIZE - 1ULL) & ~(AARCH64_PAGE_SIZE - 1ULL);
}

/* いま持っているポインタが指すものの物理アドレス。MMU を入れる前は
 * シンボルのアドレスがそのまま物理、上位 VA へ移った後は VA になる */
static uint64_t sym_pa(const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return aarch64_vm_running_high() ? aarch64_virt_to_phys(a) : a;
}

void aarch64_pmm_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t mem_end, free_base;

    for (uint64_t i = 0; i < sizeof(g_bitmap); i++) g_bitmap[i] = 0xffU;
    g_base_pa = 0;
    g_pages = 0;
    g_used = 0;

    if (!b || b->memory_size == 0) return;

    mem_end = b->memory_base + b->memory_size;

    /* **カーネルイメージとブートスタックの後ろから。** ブートスタックを
     * 空きに含めると、いま乗っているスタックを配ってしまう */
    free_base = align_up_page(sym_pa(__kernel_end)) +
                AARCH64_BOOT_CPUS * AARCH64_BOOT_STACK;
    free_base = align_up_page(free_base);
    if (free_base < b->memory_base) free_base = b->memory_base;
    if (free_base >= mem_end) return;

    g_base_pa = free_base;
    g_pages = (mem_end - free_base) / AARCH64_PAGE_SIZE;
    if (g_pages > AARCH64_PMM_MAX_PAGES) g_pages = AARCH64_PMM_MAX_PAGES;

    for (uint64_t page = 0; page < g_pages; page++) pmm_clear(page);

    /* DTB が管理領域の中にあるなら予約する。QEMU virt では RAM の先頭
     * (カーネルより手前) なので普通は当たらないが、実機では分からない */
    if (b->dtb_size != 0 && b->dtb_pa + b->dtb_size > free_base && b->dtb_pa < mem_end) {
        uint64_t s = b->dtb_pa & ~(AARCH64_PAGE_SIZE - 1ULL);
        uint64_t e = align_up_page(b->dtb_pa + b->dtb_size);
        if (s < free_base) s = free_base;
        for (uint64_t pa = s; pa < e && pa < mem_end; pa += AARCH64_PAGE_SIZE) {
            uint64_t page = (pa - free_base) / AARCH64_PAGE_SIZE;
            if (page < g_pages) { if (!pmm_test(page)) g_used++; pmm_set(page); }
        }
    }
}

/* 連続した pages 枚を確保して**物理アドレス**を返す。0 なら失敗。
 * 中身は 0 で埋める (ページテーブルに使うので、ごみが残っていると
 * 有効ビットが立ったままの descriptor を掴むことになる) */
uint64_t aarch64_pmm_alloc(uint64_t pages) {
    uint64_t run = 0, start = 0;

    if (pages == 0 || g_pages == 0) return 0;

    for (uint64_t page = 0; page < g_pages; page++) {
        if (pmm_test(page)) { run = 0; continue; }
        if (run == 0) start = page;
        if (++run == pages) {
            uint64_t pa = g_base_pa + start * AARCH64_PAGE_SIZE;
            uint8_t* p = (uint8_t*)(uintptr_t)(aarch64_vm_running_high()
                                               ? aarch64_phys_to_virt(pa) : pa);
            for (uint64_t i = 0; i < pages; i++) pmm_set(start + i);
            g_used += pages;
            for (uint64_t i = 0; i < pages * AARCH64_PAGE_SIZE; i++) p[i] = 0;
            return pa;
        }
    }
    return 0;
}

void aarch64_pmm_free(uint64_t pa, uint64_t pages) {
    if (!pa || pa < g_base_pa) return;
    uint64_t start = (pa - g_base_pa) / AARCH64_PAGE_SIZE;
    for (uint64_t i = 0; i < pages && start + i < g_pages; i++) {
        if (pmm_test(start + i)) g_used--;
        pmm_clear(start + i);
    }
}

uint64_t aarch64_pmm_base(void)  { return g_base_pa; }
uint64_t aarch64_pmm_total(void) { return g_pages; }
uint64_t aarch64_pmm_used(void)  { return g_used; }

/* ==========================================================================
 * 共有層から見た形 (M3c-2a)
 *
 * include/pmm.h が要求する形に合わせる。**戻り値は物理アドレスを void* に
 * 入れたもの** (riscv64 と同じ)。触るときは呼ぶ側が PHYS_TO_VIRT を通す。
 *
 * **g_hhdm_offset を設定するのがここでの要点。** riscv64 は 0 で済んで
 * いた (カーネルが恒等マッピングに居るため) が、aarch64 のカーネルは
 * TTBR1 の上位 VA に居るので、物理 → VA の変換に値が要る。
 * 0 のままだと、共有層が PHYS_TO_VIRT した先で必ず落ちる。
 *
 * 参照カウントは fork の copy-on-write などで使う。M3c-1 までの
 * aarch64_pmm_* はカウントを持っていないので、**こちら側で持つ**。
 * ========================================================================== */
#include "pmm.h"
#include "vmm.h"

_Static_assert(PAGE_SIZE == AARCH64_PAGE_SIZE,
               "共有層の PAGE_SIZE と aarch64 のページサイズがずれている");

static uint16_t g_refcount[AARCH64_PMM_MAX_PAGES];

static uint64_t pmm_page_index(uint64_t pa, int* ok) {
    *ok = 0;
    if (!pa || pa < g_base_pa) return 0;
    uint64_t page = (pa - g_base_pa) / AARCH64_PAGE_SIZE;
    if (page >= g_pages) return 0;
    *ok = 1;
    return page;
}

void pmm_init(void) {
    /* **カーネルが上位 VA に居ることを共有層に伝える。**
     * これを 0 のままにすると PHYS_TO_VIRT が物理を返し、
     * 恒等マッピングを外した後は必ず落ちる */
    g_hhdm_offset = AARCH64_KERNEL_VA_OFFSET;
    for (uint64_t i = 0; i < AARCH64_PMM_MAX_PAGES; i++) g_refcount[i] = 0;
    aarch64_pmm_init();
}

void* pmm_alloc(size_t pages) {
    uint64_t pa = aarch64_pmm_alloc((uint64_t)pages);
    if (!pa) return 0;
    for (size_t i = 0; i < pages; i++) {
        int ok;
        uint64_t page = pmm_page_index(pa + (uint64_t)i * AARCH64_PAGE_SIZE, &ok);
        if (ok) g_refcount[page] = 1;
    }
    return (void*)(uintptr_t)pa;
}

/* **カウントが 0 になったときだけ本当に返す。** 共有層は同じページを
 * 複数の空間に張ってから片方ずつ手放すので、無条件に返すと
 * まだ使われているページを配り直すことになる */
void pmm_free(void* addr, size_t pages) {
    uint64_t base = (uint64_t)(uintptr_t)addr;
    for (size_t i = 0; i < pages; i++) {
        int ok;
        uint64_t pa = base + (uint64_t)i * AARCH64_PAGE_SIZE;
        uint64_t page = pmm_page_index(pa, &ok);
        if (!ok) continue;
        if (g_refcount[page] > 0) {
            g_refcount[page]--;
            if (g_refcount[page] == 0) aarch64_pmm_free(pa, 1);
        }
    }
}

void pmm_incref(void* addr) {
    int ok;
    uint64_t page = pmm_page_index((uint64_t)(uintptr_t)addr, &ok);
    if (ok && g_refcount[page] < 0xffffU) g_refcount[page]++;
}

uint16_t pmm_get_ref(void* addr) {
    int ok;
    uint64_t page = pmm_page_index((uint64_t)(uintptr_t)addr, &ok);
    return ok ? g_refcount[page] : 0;
}

/* ISA DMA は x86 (16MB 未満 + 64KB 境界) の話。**aarch64 では使わない。**
 * 0 を返して「無い」と伝える */
void* pmm_get_isa_dma_page(void) { return 0; }

uint64_t pmm_get_allocated_pages(void) { return g_used; }
uint64_t pmm_get_free_pages(void)      { return g_pages - g_used; }
uint64_t pmm_get_total_pages(void)     { return g_pages; }

/* 空きページ数を 1 行で出す (P3-4)。
 *
 * **アドレス空間の解放が効いているかは、数えないと分からない。**
 * 漏れていても即座には落ちず、fork を繰り返した後で ENOMEM になるだけ
 * なので、実測値を出しておく */
void aarch64_uart_puts(const char* s);
void aarch64_uart_puthex64(uint64_t v);

void aarch64_pmm_report_free(const char* label) {
    aarch64_uart_puts(label);
    aarch64_uart_puthex64(g_pages - g_used);
    aarch64_uart_puts(" / ");
    aarch64_uart_puthex64(g_pages);
    aarch64_uart_puts("\n");
}
