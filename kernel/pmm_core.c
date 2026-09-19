#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "pmm_core.h"
#include "spinlock.h"

/* 考え方と分担は include/pmm_core.h。探し方は kernel/aarch64/pmm.c に
 * あったもの (2026-08-30 に実機のプロファイルで直した版) をそのまま移した */

static uint64_t g_base_pa;      /* 管理範囲の先頭 (物理) */
static uint64_t g_pages;        /* 管理しているページ数 */
static uint64_t g_used;         /* ビットが立っているページ数 (穴・予約を含む) */
static uint64_t g_bitmap_pa;
static uint64_t g_refcount_pa;
static uint64_t g_next_page;    /* next-fit の起点。訳は pmm_claim_locked に */

/* **ポインタは覚えておき、変わるときだけ引き直す。**aarch64 は MMU を
 * 入れる前は物理、入れた後は上位 VA で管理情報を触るので、上位 VA へ
 * 移ったところで pmm_core_rebind を呼ぶ。以前は操作のたびに引き直して
 * いたが、fork の CoW は 1 ページごとに incref / free するので、riscv64 の
 * forkbench (ws=1024) で fork+exit が 20% 遅くなった */
static uint8_t*  g_bm;
static uint16_t* g_rc;

/* 計器 (P-10)。1 回の確保で何ページ見たか */
static uint64_t g_scan_pages;
static uint64_t g_claim_calls;
static uint64_t g_claim_wrap;

/* これ以上の連なりは起点を使わず 0 から探す。16 頁 = 64KiB */
#define PMM_BIG_PAGES 16

/* **ビットマップと参照カウントを 1 本のロックで守る。**探索と確保が
 * 離れていると、2 つの CPU が同じ空きページを見つけて両方に配る。
 * 「減らして 0 なら返す」も割ると、同じページを 2 回返す */
static spinlock_t g_pmm_lock;

static void pmm_bind_locked(void) {
    g_bm = (uint8_t*)arch_pmm_meta_ptr(g_bitmap_pa);
    g_rc = (uint16_t*)arch_pmm_meta_ptr(g_refcount_pa);
}

void pmm_core_rebind(void) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    pmm_bind_locked();
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

static void pmm_set(uint64_t page)   { g_bm[page / 8U] |= (uint8_t)(1U << (page % 8U)); }
static void pmm_clear(uint64_t page) { g_bm[page / 8U] &= (uint8_t)~(1U << (page % 8U)); }
static int  pmm_test(uint64_t page)  { return (g_bm[page / 8U] >> (page % 8U)) & 1U; }

/* 範囲の中なら 1 を返してページ番号を *page に置く */
static int pmm_page_of(uint64_t pa, uint64_t* page) {
    if (pa < g_base_pa) return 0;
    *page = (pa - g_base_pa) / PAGE_SIZE;
    return *page < g_pages;
}

void pmm_core_setup(uint64_t base_pa, uint64_t pages,
                    uint64_t bitmap_pa, uint64_t refcount_pa) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    g_base_pa = base_pa;
    g_pages = pages;
    g_bitmap_pa = bitmap_pa;
    g_refcount_pa = refcount_pa;
    g_next_page = 0;
    g_scan_pages = 0;
    g_claim_calls = 0;
    g_claim_wrap = 0;
    pmm_bind_locked();

    /* **全部使用中で始める。**空きにしてよい範囲はアーキが塗る。
     * 塗られなかったところ (穴・予約) はそのまま残る */
    for (uint64_t i = 0; i < (pages + 7U) / 8U; i++) g_bm[i] = 0xffU;
    for (uint64_t i = 0; i < pages; i++) g_rc[i] = 0;
    g_used = pages;

    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_core_mark_free(uint64_t pa, uint64_t bytes) {
    uint64_t s = (pa + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    uint64_t e = (pa + bytes) & ~(PAGE_SIZE - 1ULL);
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    for (uint64_t a = s; a < e; a += PAGE_SIZE) {
        uint64_t page;
        if (!pmm_page_of(a, &page)) continue;
        if (pmm_test(page)) { pmm_clear(page); g_used--; }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_core_mark_used(uint64_t pa, uint64_t bytes) {
    uint64_t s = pa & ~(PAGE_SIZE - 1ULL);
    uint64_t e = (pa + bytes + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    for (uint64_t a = s; a < e; a += PAGE_SIZE) {
        uint64_t page;
        if (!pmm_page_of(a, &page)) continue;
        if (!pmm_test(page)) { pmm_set(page); g_used++; }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

int pmm_core_reserve_page(uint64_t pa) {
    uint64_t page;
    int ok = 0;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    if (pmm_page_of(pa, &page) && !pmm_test(page)) {
        pmm_set(page);
        g_used++;
        g_rc[page] = 1;
        ok = 1;
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return ok;
}

/* ---- 探し方 ----------------------------------------------------------------
 *
 * **次に見る場所を覚える (next-fit)。**毎回ページ 0 から走ると、使用中が
 * 増えるほど走査が伸びる。2026-08-30、Pi 4 で GCC を組みながら標本化
 * プロファイラで測ると、標本の 58% が pmm_alloc の割り込み禁止区間だった。
 *
 * **ただし解放したら起点をそこまで戻す。**戻さない素の next-fit は、穴を
 * 飛ばして前へ進み続けるので使用中が全体に薄く散らばり、同じ日に実機で
 * 1024 頁 (4MiB) の連なりが 1 つも無くなった。戻せば手前の穴から埋まるので
 * 前方が密に保たれ (first-fit の利点)、起点が穴の位置なので走査は短い
 * (next-fit の利点)。
 *
 * **大きな連なりは初めから前を見る。**小さい確保が多少散らばっても、0 から
 * 探せば手前の詰まった領域を飛ばして空きに当たる */

/* [from, to) を走査して pages 個の連なりを探す。見つけたら**先頭 + 1** を
 * 返す (0 を「なし」に使うため)。ロックを持った状態で呼ぶこと */
static uint64_t pmm_scan_locked(uint64_t from, uint64_t to, uint64_t pages) {
    uint64_t run = 0, start = 0;

    if (to > from) g_scan_pages += to - from;   /* 見込み。抜けた分は下で引く */
    for (uint64_t page = from; page < to; page++) {
        if (pmm_test(page)) { run = 0; continue; }
        if (run == 0) start = page;
        if (++run == pages) {
            g_scan_pages -= to - (page + 1);   /* 途中で見つけた分を戻す */
            return start + 1;
        }
    }
    return 0;
}

/* ロックを持った状態で呼ぶこと。ビットを立てて g_used を進めるだけで、
 * 参照カウントにも中身にも触らない。失敗は 0 */
static uint64_t pmm_claim_locked(uint64_t pages) {
    uint64_t start, tail;

    if (pages == 0 || g_pages == 0) return 0;
    if (g_next_page >= g_pages) g_next_page = 0;
    g_claim_calls++;

    if (pages >= PMM_BIG_PAGES) {
        start = pmm_scan_locked(0, g_pages, pages);
        if (!start) return 0;
        start--;
        for (uint64_t i = 0; i < pages; i++) pmm_set(start + i);
        g_used += pages;
        return g_base_pa + start * PAGE_SIZE;
    }

    /* 起点から末尾まで。普通はここで当たる */
    start = pmm_scan_locked(g_next_page, g_pages, pages);
    if (!start) {
        /* **一周する。**起点をまたぐ連なりも拾えるよう、終端を pages - 1
         * だけ伸ばす (g_pages で頭打ち)。ここに来るのは空きが尽きかけた時だけ */
        g_claim_wrap++;
        tail = g_next_page + pages - 1;
        if (tail > g_pages) tail = g_pages;
        start = pmm_scan_locked(0, tail, pages);
        if (!start) return 0;
    }
    start--;

    for (uint64_t i = 0; i < pages; i++) pmm_set(start + i);
    g_used += pages;
    g_next_page = start + pages;
    return g_base_pa + start * PAGE_SIZE;
}

/* ロックを持った状態で呼ぶこと */
static void pmm_release_locked(uint64_t pa, uint64_t pages) {
    uint64_t start;

    if (!pmm_page_of(pa, &start)) return;
    /* **起点をここまで戻す。**手前の穴から埋め直させて、前方を密に保つ */
    if (start < g_next_page) g_next_page = start;
    for (uint64_t i = 0; i < pages && start + i < g_pages; i++) {
        if (pmm_test(start + i)) g_used--;
        pmm_clear(start + i);
    }
}

uint64_t pmm_core_claim(uint64_t pages) {
    uint64_t pa;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    pa = pmm_claim_locked(pages);
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return pa;
}

void pmm_core_release(uint64_t pa, uint64_t pages) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    pmm_release_locked(pa, pages);
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint64_t pmm_core_base(void)  { return g_base_pa; }
uint64_t pmm_core_total(void) { return g_pages; }
uint64_t pmm_core_used(void)  { return g_used; }

void pmm_core_take_scan_stat(struct pmm_core_scan_stat* out) {
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);
    out->calls = g_claim_calls;
    out->scanned = g_scan_pages;
    out->wraps = g_claim_wrap;
    out->next_page = g_next_page;
    g_claim_calls = 0;
    g_scan_pages = 0;
    g_claim_wrap = 0;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

/* ---- 共有層から見た形 (include/pmm.h) ------------------------------------
 *
 * 戻り値は物理アドレスを void* に入れたもの。中身を触るときは呼ぶ側が
 * PHYS_TO_VIRT を通す */

/* **確保と参照カウントの初期化を 1 つのロックの中で済ませる。**分けると、
 * 参照カウントが 1 になる前のページを別の CPU の pmm_free が見に来る
 * 余地が残る。後始末 (aarch64 の 0 埋め) だけロックの外 */
void* pmm_alloc(size_t pages) {
    uint64_t pa;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    pa = pmm_claim_locked((uint64_t)pages);
    if (pa) {
        uint64_t first = (pa - g_base_pa) / PAGE_SIZE;
        for (size_t i = 0; i < pages; i++) g_rc[first + i] = 1;
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    if (!pa) return 0;
    arch_pmm_after_alloc(pa, (uint64_t)pages);
    return (void*)(uintptr_t)pa;
}

/* **参照カウントが 0 になったときだけ本当に返す。**共有層は同じページを
 * 複数の空間に張ってから片方ずつ手放す (fork の CoW) */
void pmm_free(void* addr, size_t pages) {
    uint64_t base = (uint64_t)(uintptr_t)addr;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    for (size_t i = 0; i < pages; i++) {
        uint64_t pa = base + (uint64_t)i * PAGE_SIZE;
        uint64_t page;
        if (!pmm_page_of(pa, &page)) continue;
        if (g_rc[page] > 0) {
            g_rc[page]--;
            if (g_rc[page] == 0) pmm_release_locked(pa, 1);
        }
    }
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

void pmm_incref(void* addr) {
    uint64_t page;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    if (pmm_page_of((uint64_t)(uintptr_t)addr, &page) && g_rc[page] < 0xffffU) g_rc[page]++;
    spin_unlock_irqrestore(&g_pmm_lock, flags);
}

uint16_t pmm_get_ref(void* addr) {
    uint64_t page;
    uint16_t ref = 0;
    uint64_t flags = spin_lock_irqsave(&g_pmm_lock);

    if (pmm_page_of((uint64_t)(uintptr_t)addr, &page)) ref = g_rc[page];
    spin_unlock_irqrestore(&g_pmm_lock, flags);
    return ref;
}

/* **使用中 = 全体 - 空き。**以前は x86 だけ「pmm_alloc で配った数」を
 * 返していた (aarch64 / riscv64 はビットの数)。sysinfo は
 * totalram = 使用中 + 空き なので、どちらでも全体は管理範囲になる */
uint64_t pmm_get_allocated_pages(void) { return g_used; }
uint64_t pmm_get_free_pages(void)      { return g_pages - g_used; }
uint64_t pmm_get_total_pages(void)     { return g_pages; }
