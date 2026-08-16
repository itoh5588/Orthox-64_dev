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

/* 管理情報 (ビットマップと refcount) は**管理対象の RAM から切り出す**。
 *
 * **以前は静的配列だった。** 4KB x 1048576 = 4GB ぶんを .bss に固定で置き、
 * RAM が何 MB の機械でも bitmap 128KB + refcount 2048KB = 2176KB を必ず食う。
 * しかも **4GB を超える機械は上限で切り捨てていた** (Pi 4 の 8GB モデル)。
 *
 * いまは実際の RAM の広さから必要量を計算し、**空き領域の先頭から取って
 * 自分で使用済みにする**。上限が消え、.bss も 2176KB 減る。
 *
 *   1 ページあたり  bitmap 1/8 バイト + refcount 2 バイト = 約 2.125 バイト
 *   4GB (1048576 ページ)  約 2.1MB    8GB (2097152 ページ)  約 4.3MB
 *
 * **置き場は空き領域の先頭 = カーネルとブートスタックの直後。** そこは
 * カーネル自身が載っている実在の RAM なので、Pi 4 の「RAM の穴」には
 * 当たらない。当たっていないことは init の中で確かめる (穴の上に管理情報を
 * 置くと、ファームウェアの持ち物を黙って壊すため)。 */

extern char __kernel_end[];

static uint64_t g_base_pa;      /* 管理領域の先頭 (物理) */
static uint64_t g_pages;        /* 管理しているページ数 */
static uint64_t g_used;
static uint64_t g_bitmap_pa;    /* ビットマップの物理アドレス (管理領域の中) */
static uint64_t g_refcount_pa;  /* refcount の物理アドレス (同上) */
static uint64_t g_meta_pages;   /* 管理情報が占めるページ数 */
static int      g_meta_fault;   /* 管理情報を置けなかった (穴の上だった) */

/* **管理情報は物理アドレスで持つ。**
 *
 * pmm_init は MMU を入れる前 (物理アドレスで走っている) に動くが、
 * 以後の alloc/free は上位 VA へ移った後から来る。静的配列ならリンカが
 * 面倒を見てくれたが、切り出した領域は自分で変換しないと、MMU を入れた
 * 瞬間に届かなくなる。**触るたびに今の走り方で変換する。** */
static void* pmm_meta_ptr(uint64_t pa) {
    return (void*)(uintptr_t)(aarch64_vm_running_high() ? aarch64_phys_to_virt(pa) : pa);
}
static uint8_t*  pmm_bitmap(void)   { return (uint8_t*)pmm_meta_ptr(g_bitmap_pa); }
static uint16_t* pmm_refcount(void) { return (uint16_t*)pmm_meta_ptr(g_refcount_pa); }

static void pmm_set(uint64_t page)   { pmm_bitmap()[page / 8U] |= (uint8_t)(1U << (page % 8U)); }
static void pmm_clear(uint64_t page) { pmm_bitmap()[page / 8U] &= (uint8_t)~(1U << (page % 8U)); }
static int  pmm_test(uint64_t page)  { return (pmm_bitmap()[page / 8U] >> (page % 8U)) & 1U; }

static uint64_t align_up_page(uint64_t v) {
    return (v + AARCH64_PAGE_SIZE - 1ULL) & ~(AARCH64_PAGE_SIZE - 1ULL);
}

/* いま持っているポインタが指すものの物理アドレス。MMU を入れる前は
 * シンボルのアドレスがそのまま物理、上位 VA へ移った後は VA になる */
static uint64_t sym_pa(const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return aarch64_vm_running_high() ? aarch64_virt_to_phys(a) : a;
}

/* pa が DTB の言う実在レンジの中か。**レンジが取れていない (既定値に退いた)
 * ときは全体を実在とみなす** — 従来の振る舞いに合わせる */
static int pmm_pa_in_ranges(const aarch64_boot_info_t* b, uint64_t pa) {
    if (b->mem_range_count == 0) return 1;
    for (uint32_t r = 0; r < b->mem_range_count; r++) {
        if (pa >= b->mem_range_base[r] &&
            pa < b->mem_range_base[r] + b->mem_range_size[r]) return 1;
    }
    return 0;
}

void aarch64_pmm_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t mem_end, free_base, bitmap_bytes, meta_bytes;

    g_base_pa = 0;
    g_pages = 0;
    g_used = 0;
    g_bitmap_pa = 0;
    g_refcount_pa = 0;
    g_meta_pages = 0;
    g_meta_fault = 0;

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

    /* ---- 管理情報の置き場を決める ----------------------------------------
     *
     * **大きさはページ数から決まり、置き場は管理領域の先頭で固定**なので、
     * 「ページ数 -> 大きさ -> 使用済みにする」の順で回る。
     * refcount は uint16_t なので、ビットマップの後ろを 8 バイト境界に揃える */
    bitmap_bytes = (g_pages + 7U) / 8U;
    bitmap_bytes = (bitmap_bytes + 7U) & ~7ULL;
    meta_bytes   = bitmap_bytes + g_pages * sizeof(uint16_t);
    g_meta_pages = align_up_page(meta_bytes) / AARCH64_PAGE_SIZE;

    /* 管理情報だけで RAM を食い切るなら、その RAM は使い物にならない */
    if (g_meta_pages >= g_pages) {
        g_base_pa = 0; g_pages = 0; g_meta_pages = 0;
        return;
    }

    /* **書き込む前に、置き場が実在の RAM か確かめる。** 穴 (ファームウェアや
     * GPU の持ち物) の上に置くと、黙って他人の領域を壊す。
     * カーネル自身が載っている場所の直後なので普通は当たらないが、
     * 当たったときに気づけないほうが困る */
    for (uint64_t i = 0; i < g_meta_pages; i++) {
        if (!pmm_pa_in_ranges(b, free_base + i * AARCH64_PAGE_SIZE)) {
            g_meta_fault = 1;
            g_base_pa = 0; g_pages = 0; g_meta_pages = 0;
            return;
        }
    }

    g_bitmap_pa   = free_base;
    g_refcount_pa = free_base + bitmap_bytes;

    /* **全部使用済みで始める。** 穴はこのまま残る */
    {
        uint8_t* bm = pmm_bitmap();
        for (uint64_t i = 0; i < bitmap_bytes; i++) bm[i] = 0xffU;
    }
    {
        uint16_t* rc = pmm_refcount();
        for (uint64_t i = 0; i < g_pages; i++) rc[i] = 0;
    }

    /* **穴は使用済みのまま残す。**
     *
     * Raspberry Pi 4 (4GB) は RAM が 2 つに割れていて、間に
     * ファームウェア/GPU の予約領域がある。memory_base/size は穴を含む
     * 全体を指す (HHDM がそれを使う) が、**配ってよいのは実在するレンジだけ**。
     *
     * ビットマップは 0xff (全部使用済み) で初期化してあるので、
     * レンジの中だけを空きにすれば穴は自動的に残る。
     *
     * mem_range_count == 0 は DTB から取れず既定値に退いた場合。
     * そのときは従来どおり全体を空きとして扱う */
    if (b->mem_range_count == 0) {
        for (uint64_t page = 0; page < g_pages; page++) pmm_clear(page);
    } else {
        uint64_t usable = 0;
        for (uint32_t r = 0; r < b->mem_range_count; r++) {
            uint64_t rs = b->mem_range_base[r];
            uint64_t re = rs + b->mem_range_size[r];
            if (rs < free_base) rs = free_base;
            if (re > mem_end) re = mem_end;
            for (uint64_t pa = rs; pa < re; pa += AARCH64_PAGE_SIZE) {
                uint64_t page = (pa - g_base_pa) / AARCH64_PAGE_SIZE;
                if (page < g_pages && pmm_test(page)) { pmm_clear(page); usable++; }
            }
        }
        /* **穴のぶんを最初から「使用済み」として数える。** そうしないと
         * 「pmm : 全体 (使用 0)」と出て、実際より多く使えるように見える */
        g_used = g_pages - usable;
    }

    /* **管理情報が載っているページを使用済みに戻す。**
     *
     * 上のレンジ走査は「実在する RAM は全部空き」と塗るので、管理情報の
     * ぶんも空きにされている。ここで取り返さないと、**ビットマップ自身を
     * ページとして配ってしまう** (次の alloc が管理情報を上書きする)。
     *
     * mem_range_count == 0 の道でも同じことが起きるので、if の外に置く */
    for (uint64_t i = 0; i < g_meta_pages; i++) {
        if (!pmm_test(i)) { pmm_set(i); g_used++; }
    }

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

/* 管理情報が何ページを占めているか。**起動ログに出して確かめる** —
 * 静的配列をやめた以上、「切り出せた」ことは数字で見えないと分からない */
uint64_t aarch64_pmm_meta_pages(void) { return g_meta_pages; }

/* 管理情報を実在の RAM に置けなかった。**この場合 pmm は 0 ページで
 * 返している**ので、起動ログで理由が分かるようにする */
int aarch64_pmm_meta_fault(void) { return g_meta_fault; }

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
 * **実体は管理対象の RAM から切り出した領域** (ファイル冒頭を参照)。
 * ページ数ぶんの uint16_t が並んでいて、pmm_refcount() が先頭を返す。
 * ========================================================================== */
#include "pmm.h"
#include "vmm.h"

_Static_assert(PAGE_SIZE == AARCH64_PAGE_SIZE,
               "共有層の PAGE_SIZE と aarch64 のページサイズがずれている");

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
    /* **refcount の 0 埋めは aarch64_pmm_init の中でやる。**
     * 実体は切り出した領域なので、置き場が決まる前には触れない */
    aarch64_pmm_init();
}

void* pmm_alloc(size_t pages) {
    uint64_t pa = aarch64_pmm_alloc((uint64_t)pages);
    if (!pa) return 0;
    for (size_t i = 0; i < pages; i++) {
        int ok;
        uint64_t page = pmm_page_index(pa + (uint64_t)i * AARCH64_PAGE_SIZE, &ok);
        if (ok) pmm_refcount()[page] = 1;
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
        uint16_t* rc = pmm_refcount();
        if (rc[page] > 0) {
            rc[page]--;
            if (rc[page] == 0) aarch64_pmm_free(pa, 1);
        }
    }
}

void pmm_incref(void* addr) {
    int ok;
    uint64_t page = pmm_page_index((uint64_t)(uintptr_t)addr, &ok);
    if (ok && pmm_refcount()[page] < 0xffffU) pmm_refcount()[page]++;
}

uint16_t pmm_get_ref(void* addr) {
    int ok;
    uint64_t page = pmm_page_index((uint64_t)(uintptr_t)addr, &ok);
    return ok ? pmm_refcount()[page] : 0;
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
