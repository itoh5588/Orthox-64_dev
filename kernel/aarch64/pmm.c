/*
 * 物理メモリ管理 (M3b-2)。ビットマップで 4KB ページを配る。
 *
 * **配り方は共通層 (kernel/pmm_core.c、2026-09-19)。**ここは管理する範囲と
 * 管理情報の置き場を決め、RAM の穴を使用済みのまま残す。
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
#include "pmm_core.h"

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
static uint64_t g_meta_pages;   /* 管理情報が占めるページ数 */
static int      g_meta_fault;   /* 管理情報を置けなかった (穴の上だった) */

/* **管理情報は物理アドレスで持つ。**
 *
 * pmm_init は MMU を入れる前 (物理アドレスで走っている) に動くが、
 * 以後の alloc/free は上位 VA へ移った後から来る。静的配列ならリンカが
 * 面倒を見てくれたが、切り出した領域は自分で変換しないと、MMU を入れた
 * 瞬間に届かなくなる。**引くたびに今の走り方で変換する。**
 * 共通層は pmm_core_setup と pmm_core_rebind (上位 VA へ移ったとき。
 * kernel/aarch64/boot.c の aarch64_boot_continue) でこれを呼ぶ */
void* arch_pmm_meta_ptr(uint64_t pa) {
    return (void*)(uintptr_t)(aarch64_vm_running_high() ? aarch64_phys_to_virt(pa) : pa);
}

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

    /* **全部使用済みで始める (pmm_core_setup)。** 穴はこのまま残る */
    pmm_core_setup(free_base, g_pages, free_base, free_base + bitmap_bytes);

    /* **穴は使用済みのまま残す。**
     *
     * Raspberry Pi 4 (4GB) は RAM が 2 つに割れていて、間に
     * ファームウェア/GPU の予約領域がある。memory_base/size は穴を含む
     * 全体を指す (HHDM がそれを使う) が、**配ってよいのは実在するレンジだけ**。
     *
     * 全部使用済みで始めているので、レンジの中だけを空きにすれば穴は
     * 自動的に残る。穴のぶんは最初から「使用済み」として数えられる
     * (そうしないと「pmm : 全体 (使用 0)」と出て、実際より多く使えるように
     * 見える)。
     *
     * mem_range_count == 0 は DTB から取れず既定値に退いた場合。
     * そのときは従来どおり全体を空きとして扱う */
    if (b->mem_range_count == 0) {
        pmm_core_mark_free(free_base, g_pages * AARCH64_PAGE_SIZE);
    } else {
        for (uint32_t r = 0; r < b->mem_range_count; r++) {
            uint64_t rs = b->mem_range_base[r];
            uint64_t re = rs + b->mem_range_size[r];
            if (rs < free_base) rs = free_base;
            if (re > mem_end) re = mem_end;
            if (re > rs) pmm_core_mark_free(rs, re - rs);
        }
    }

    /* **管理情報が載っているページを使用済みに戻す。**
     *
     * 上のレンジ走査は「実在する RAM は全部空き」と塗るので、管理情報の
     * ぶんも空きにされている。ここで取り返さないと、**ビットマップ自身を
     * ページとして配ってしまう** (次の alloc が管理情報を上書きする)。
     *
     * mem_range_count == 0 の道でも同じことが起きるので、if の外に置く */
    pmm_core_mark_used(free_base, g_meta_pages * AARCH64_PAGE_SIZE);

    /* DTB が管理領域の中にあるなら予約する。QEMU virt では RAM の先頭
     * (カーネルより手前) なので普通は当たらないが、実機では分からない */
    if (b->dtb_size != 0 && b->dtb_pa + b->dtb_size > free_base && b->dtb_pa < mem_end) {
        pmm_core_mark_used(b->dtb_pa, b->dtb_size);
    }
}

/* ---- 配り方は共通層 (kernel/pmm_core.c) -------------------------------------
 *
 * ロック、next-fit (解放で起点を戻す、大きな連なりは先頭から)、走査の計器は
 * 2026-09-19 に共通層へ移した。x86 / riscv64 は毎回ページ 0 から探していた
 * ので、こちらで 08-30 に直したものを 3 アーキで使う */

/* ロックの外で呼ぶ。**MMU の前後どちらでも触れるように経路を選ぶ** */
/* **8 バイト単位で埋める。**
 *
 * 元は `p[i] = 0` のバイトループで、-O2 でも strb 2 本の繰り返しにしか
 * ならなかった (4KiB あたり 2048 周)。2026-08-30 の実機プロファイルで、
 * next-fit を入れたあとの pmm_alloc の滞在先はこの 0 埋めループだった。
 * ページは 4KiB 境界に揃っているので 8 バイト書きで安全に埋まる。 */
static void pmm_zero_pages(uint64_t pa, uint64_t pages) {
    uint64_t* p = (uint64_t*)(uintptr_t)(aarch64_vm_running_high()
                                         ? aarch64_phys_to_virt(pa) : pa);
    uint64_t n = pages * (AARCH64_PAGE_SIZE / 8);
    for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

/* 共有層の pmm_alloc で取ったページも 0 で埋める (以前からの振る舞い) */
void arch_pmm_after_alloc(uint64_t pa, uint64_t pages) {
    pmm_zero_pages(pa, pages);
}

/* 連続した pages 枚を確保して**物理アドレス**を返す。0 なら失敗。
 * 中身は 0 で埋める (ページテーブルに使うので、ごみが残っていると
 * 有効ビットが立ったままの descriptor を掴むことになる)。
 * 参照カウントには触らない (共有層の外で持つもの用) */
uint64_t aarch64_pmm_alloc(uint64_t pages) {
    uint64_t pa = pmm_core_claim(pages);
    if (pa) pmm_zero_pages(pa, pages);
    return pa;
}

void aarch64_pmm_free(uint64_t pa, uint64_t pages) {
    pmm_core_release(pa, pages);
}

uint64_t aarch64_pmm_base(void)  { return g_base_pa; }
/* 60 秒ごとの計器。区間ごとに見たいので、出したら 0 に戻す */
void aarch64_pmm_scan_report(void) {
    struct pmm_core_scan_stat st;

    pmm_core_take_scan_stat(&st);
    if (st.calls == 0) return;

    aarch64_uart_puts("[pmm] 60s  alloc ");
    aarch64_uart_putdec64(st.calls);
    aarch64_uart_puts(" times  scanned ");
    aarch64_uart_putdec64(st.scanned);
    aarch64_uart_puts(" pages  per commit ");
    aarch64_uart_putdec64(st.scanned / st.calls);
    aarch64_uart_puts(" pages  2 laps ");
    aarch64_uart_putdec64(st.wraps);
    aarch64_uart_puts(" times  used ");
    aarch64_uart_putdec64(pmm_core_used());
    aarch64_uart_puts("/");
    aarch64_uart_putdec64(pmm_core_total());
    aarch64_uart_puts(" pages  origin ");
    aarch64_uart_putdec64(st.next_page);
    aarch64_uart_puts("\n");
}

uint64_t aarch64_pmm_total(void) { return pmm_core_total(); }
uint64_t aarch64_pmm_used(void)  { return pmm_core_used(); }

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
 * 参照カウントは fork の copy-on-write などで使う。読み書きは共通層
 * (kernel/pmm_core.c) がする。**実体は管理対象の RAM から切り出した領域**
 * (ファイル冒頭を参照) で、ビットマップの後ろにページ数ぶんの uint16_t が
 * 並ぶ。
 * ========================================================================== */
#include "pmm.h"
#include "vmm.h"

_Static_assert(PAGE_SIZE == AARCH64_PAGE_SIZE,
               "shared-layer PAGE_SIZE differs from aarch64 page size");

void pmm_init(void) {
    /* **カーネルが上位 VA に居ることを共有層に伝える。**
     * これを 0 のままにすると PHYS_TO_VIRT が物理を返し、
     * 恒等マッピングを外した後は必ず落ちる */
    g_hhdm_offset = AARCH64_KERNEL_VA_OFFSET;
    /* **refcount の 0 埋めは aarch64_pmm_init の中でやる。**
     * 実体は切り出した領域なので、置き場が決まる前には触れない */
    aarch64_pmm_init();
}

/* pmm_alloc / pmm_free / pmm_incref / pmm_get_ref と使用量の問い合わせは
 * 共通層 (kernel/pmm_core.c) にある */

/* ISA DMA は x86 (16MB 未満 + 64KB 境界) の話。**aarch64 では使わない。**
 * 0 を返して「無い」と伝える */
void* pmm_get_isa_dma_page(void) { return 0; }

/* 空きページ数を 1 行で出す (P3-4)。
 *
 * **アドレス空間の解放が効いているかは、数えないと分からない。**
 * 漏れていても即座には落ちず、fork を繰り返した後で ENOMEM になるだけ
 * なので、実測値を出しておく */
void aarch64_uart_puts(const char* s);
void aarch64_uart_puthex64(uint64_t v);

void aarch64_pmm_report_free(const char* label) {
    aarch64_uart_puts(label);
    aarch64_uart_puthex64(pmm_core_total() - pmm_core_used());
    aarch64_uart_puts(" / ");
    aarch64_uart_puthex64(pmm_core_total());
    aarch64_uart_puts("\n");
}
