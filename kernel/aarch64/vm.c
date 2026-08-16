/*
 * AArch64 の MMU (M2)。恒等マッピング (VA == PA) で有効化するところまで。
 *
 * riscv64 は satp にテーブルの物理アドレスを書いて Sv39 (3 段) を使った。
 * AArch64 で違うのは主に 3 点:
 *
 *   1. ベースレジスタが 2 本ある (TTBR0_EL1 = 下位側 / TTBR1_EL1 = 上位側)。
 *      ここでは カーネルが 0x40200000 = 下位側に居るので TTBR0 だけを使い、
 *      TTBR1 は TCR_EL1.EPD1 = 1 で「歩かない」ことにしてある。
 *      **カーネルを上位半分へ移すのは M3b。** リンク先の変更を伴ううえ、
 *      それをやるまで「プロセスごとに TTBR0 を差し替える」ができない
 *      (差し替えた瞬間、例外で戻ってきたときカーネルが見えなくなる)。
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
#include <stddef.h>
#include "aarch64/boot.h"
#include "aarch64/fb.h"
#include "aarch64/vm.h"
#include "aarch64/usermode.h"
/* **ユーザーページは参照カウント付きの pmm_alloc / pmm_free で扱う。**
 * aarch64_pmm_alloc (生) で取ると refcount が 0 のままになり、pmm_free は
 * 「カウントが 0 より大きいときだけ減らす」作りなので**永久に解放されない**。
 * elf.c / task_exec.c / linux_syscall.c も pmm_alloc を使っている */
#include "pmm.h"

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
extern char __user_text_start[];
extern char __user_text_end[];
extern char __user_data_start[];
extern char __user_data_end[];

/* EL0 の入口 (kernel/aarch64/user_blob.S)。.user_text の中にある */
extern char aarch64_user_entry[];

#define AARCH64_PAGE_SIZE   0x1000ULL
#define AARCH64_BLOCK_SIZE  0x200000ULL          /* 2MB。L2 の 1 エントリ */
#define AARCH64_PTES        512

/* descriptor のビット (PTE_*) と MAIR の枠は include/aarch64/vm.h に移した
 * (M3c-2a)。共有層向けの arch_vm_* が同じビットを組み立てるため */

/* ---- MAIR_EL1 の枠 -------------------------------------------------------
 *
 *   0 番: Device-nGnRnE (0x00)  MMIO 用。並べ替えも結合もキャッシュもしない
 *   1 番: Normal, Inner/Outer Write-Back Read/Write-Allocate (0xff)  RAM 用
 *
 * デバイスを 1 番で張ると、書き込みがキャッシュに溜まって UART に届かない、
 * 読み込みが古い値を返す、といった形で壊れる。QEMU では見逃せても実機で出る。 */
#define MAIR_ATTR_DEVICE_nGnRnE 0x00ULL
#define MAIR_ATTR_NORMAL_WB     0xffULL
#define MAIR_ATTR_NORMAL_NC     0x44ULL   /* outer NC / inner NC。画面用 */
/* MAIR_IDX_DEVICE / MAIR_IDX_NORMAL は include/aarch64/vm.h */
#define MAIR_EL1_VALUE  ((MAIR_ATTR_DEVICE_nGnRnE << (8 * MAIR_IDX_DEVICE)) | \
                         (MAIR_ATTR_NORMAL_WB     << (8 * MAIR_IDX_NORMAL))  | \
                         (MAIR_ATTR_NORMAL_NC     << (8 * MAIR_IDX_NORMAL_NC)))

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
/* TCR_EPD1 (bit 23) は「TTBR1 を歩かない」。M2 では立てていたが、
 * **M3b でカーネルを上位半分へ移したので外す。** 立てたままだと、
 * 上位 VA へ飛んだ瞬間に translation fault になって沈黙する */
#define TCR_TG1_4K      (2ULL << 30)             /* TG1: 0b10 = 4KB (TG0 と違う) */
#define TCR_IPS_40BIT   (2ULL << 32)             /* 物理アドレス 40bit (1TB) */

#define AARCH64_VA_BITS 39
#define TCR_EL1_VALUE   (TCR_T0SZ(64 - AARCH64_VA_BITS) | \
                         TCR_IRGN0_WBWA | TCR_ORGN0_WBWA | TCR_SH0_INNER | \
                         TCR_TG0_4K | \
                         TCR_T1SZ(64 - AARCH64_VA_BITS) | TCR_TG1_4K | \
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
#define VM_ATTR_NORMAL  (AARCH64_PTE_ATTRINDX(MAIR_IDX_NORMAL) | AARCH64_PTE_SH_INNER | AARCH64_PTE_AF)
#define VM_ATTR_DEVICE  (AARCH64_PTE_ATTRINDX(MAIR_IDX_DEVICE) | AARCH64_PTE_AF)

#define VM_KERNEL_TEXT  (VM_ATTR_NORMAL | AARCH64_PTE_AP_RO_EL1 | AARCH64_PTE_UXN)
#define VM_KERNEL_RO    (VM_ATTR_NORMAL | AARCH64_PTE_AP_RO_EL1 | AARCH64_PTE_UXN | AARCH64_PTE_PXN)
#define VM_KERNEL_RW    (VM_ATTR_NORMAL | AARCH64_PTE_AP_RW_EL1 | AARCH64_PTE_UXN | AARCH64_PTE_PXN)
#define VM_DEVICE_RW    (VM_ATTR_DEVICE | AARCH64_PTE_AP_RW_EL1 | AARCH64_PTE_UXN | AARCH64_PTE_PXN)
/* フレームバッファ。**Normal NC** — 速さと可視性の折り合い (vm.h の MAIR_IDX_NORMAL_NC) */
#define VM_ATTR_NORMAL_NC (AARCH64_PTE_ATTRINDX(MAIR_IDX_NORMAL_NC) | AARCH64_PTE_AF)
#define VM_FB_RW        (VM_ATTR_NORMAL_NC | AARCH64_PTE_AP_RW_EL1 | AARCH64_PTE_UXN | AARCH64_PTE_PXN)

/* ---- EL0 から使えるページ (M3a) -----------------------------------------
 *
 * M3a ではアドレス空間を分けていない。カーネルと同じテーブルに、
 * **AP (権限ビット) だけを変えて**張る。分けるには TTBR1 でカーネルを
 * 上位半分へ移すのが先で、それは M3b。
 *
 * **PXN を必ず立てること。** EL1 がユーザーのコードを実行できてしまうと、
 * ユーザーが書いた命令をカーネル権限で走らせる道ができる。
 * UXN と PXN は別のビットで、片方だけでは塞げない。 */
#define VM_USER_TEXT    (VM_ATTR_NORMAL | AARCH64_PTE_AP_RO_EL0 | AARCH64_PTE_PXN | AARCH64_PTE_nG)
#define VM_USER_RW      (VM_ATTR_NORMAL | AARCH64_PTE_AP_RW_EL0 | AARCH64_PTE_UXN | AARCH64_PTE_PXN | AARCH64_PTE_nG)
/* ユーザーに見せるフレームバッファ。**カーネル側と同じ Normal NC。**
 * ここだけ Normal WB にすると、DOOM が書いた絵がキャッシュに留まる */
#define VM_USER_FB      (VM_ATTR_NORMAL_NC | AARCH64_PTE_AP_RW_EL0 | AARCH64_PTE_UXN | AARCH64_PTE_PXN | AARCH64_PTE_nG)

/* 移行のあいだだけ TTBR0 に張る恒等マッピング。**実行もできる必要がある。**
 * MMU を入れた瞬間、PC はまだ物理を指しているので、そこが実行可能で
 * なければ即座に落ちる。上位 VA へ飛んだ後に外す */
#define VM_IDENT_RWX    (VM_ATTR_NORMAL | AARCH64_PTE_AP_RW_EL1)
#define VM_IDENT_DEVICE (VM_ATTR_DEVICE | AARCH64_PTE_AP_RW_EL1 | AARCH64_PTE_UXN | AARCH64_PTE_PXN)

/* ---- テーブルの置き場 ----------------------------------------------------
 *
 * **pmm から取る (M3b-2)。** M2 の時点では pmm が無かったので .bss に
 * 固定の枠を置いていたが、プロセスごとにテーブルを作るようになると
 * 枚数が読めない。 */
uint64_t aarch64_pmm_alloc(uint64_t pages);
void aarch64_pmm_free(uint64_t pa, uint64_t pages);

static unsigned g_tables_used;
static uint64_t g_kernel_root_pa;    /* TTBR1。カーネルの上位 VA */
static uint64_t g_ident_root_pa;     /* TTBR0。移行のあいだだけ使う恒等 */
static uint64_t g_user_root_pa;      /* TTBR0。上位へ飛んだ後のユーザー空間 */
static uint64_t g_build_root_pa;     /* いま組み立て中のテーブル */
static int g_vm_failed;

/* ---- 物理アドレスと「いま触れるポインタ」の変換 --------------------------
 *
 * **ページテーブルの中身は常に物理アドレス。** 一方、それを C から触るには
 * 「いまの世界で有効なアドレス」が要る。MMU を入れる前は物理がそのまま
 * 使えるが、上位 VA へ飛んだ後は phys_to_virt を通す必要がある。
 *
 * ここを間違えると、恒等マッピングを外した瞬間にテーブルを辿れなくなる。 */
static uint64_t* aarch64_vm_table_ptr(uint64_t table_pa) {
    if (aarch64_vm_running_high()) {
        return (uint64_t*)(uintptr_t)aarch64_phys_to_virt(table_pa);
    }
    return (uint64_t*)(uintptr_t)table_pa;
}

/* 逆向き。いま持っているポインタが指すものの物理アドレス */
static uint64_t aarch64_vm_ptr_pa(const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    return aarch64_vm_running_high() ? aarch64_virt_to_phys(a) : a;
}

/* 戻り値はテーブルの**物理アドレス**。0 なら失敗。
 * pmm が 0 埋めして返すので、ここでの初期化は要らない */
static uint64_t aarch64_vm_alloc_table(void) {
    uint64_t pa = aarch64_pmm_alloc(1);
    if (!pa) {
        aarch64_uart_puts("  vm: テーブル用のページを確保できない\n");
        g_vm_failed = 1;
        return 0;
    }
    g_tables_used++;
    return pa;
}

/* VA から各段のインデックスを取り出す。level 1 = bits 38:30、
 * level 2 = 29:21、level 3 = 20:12。riscv64 の VPN と考え方は同じ */
static uint64_t aarch64_vm_index(uint64_t va, int level) {
    return (va >> (12 + (3 - level) * 9)) & (AARCH64_PTES - 1);
}

/* level の段のテーブルを辿り、無ければ作る。引数も戻り値も物理アドレス */
static uint64_t aarch64_vm_next_table(uint64_t table_pa, uint64_t va, int level) {
    uint64_t* table = aarch64_vm_table_ptr(table_pa);
    uint64_t index = aarch64_vm_index(va, level);
    uint64_t entry = table[index];
    uint64_t next_pa;

    if (entry & AARCH64_PTE_VALID) {
        /* 既にブロックが張られている所を細かく割ろうとしている。
         * いまの張り方では起きないが、黙って壊さず気づけるようにする */
        if ((entry & AARCH64_PTE_TABLE) == 0) {
            aarch64_uart_puts("  vm: block/table conflict at ");
            aarch64_uart_puthex64(va);
            aarch64_uart_puts("\n");
            g_vm_failed = 1;
            return 0;
        }
        return entry & AARCH64_PTE_ADDR_MASK;
    }

    next_pa = aarch64_vm_alloc_table();
    if (!next_pa) return 0;
    table[index] = (next_pa & AARCH64_PTE_ADDR_MASK) | AARCH64_PTE_VALID | AARCH64_PTE_TABLE;
    return next_pa;
}

/* **作らずに**辿る。無ければ 0。unmap / 属性の差し替えで使う。
 * aarch64_vm_next_table は無ければ作ってしまうので、外す側には使えない */
static uint64_t aarch64_vm_walk_existing(uint64_t table_pa, uint64_t va, int level) {
    uint64_t* table = aarch64_vm_table_ptr(table_pa);
    uint64_t entry = table[aarch64_vm_index(va, level)];
    if ((entry & AARCH64_PTE_VALID) == 0) return 0;
    if ((entry & AARCH64_PTE_TABLE) == 0) return 0;   /* ブロック。細かくは辿れない */
    return entry & AARCH64_PTE_ADDR_MASK;
}

/* 1 ページ分の TLB を捨てる。**引数は VA の bits[55:12]。**
 * tlbi vae1 はページ番号を取るので、そのまま VA を渡すと別の所を捨てる */
static void aarch64_vm_flush_va(uint64_t va) {
    __asm__ volatile("dsb ishst");
    __asm__ volatile("tlbi vaae1is, %0" :: "r"(va >> 12));
    __asm__ volatile("dsb ish");
    __asm__ volatile("isb");
}

/* 4KB ページを 1 枚張る。L3 の descriptor は AARCH64_PTE_TABLE (0b11) が必須 */
static void aarch64_vm_map_page(uint64_t va, uint64_t pa, uint64_t attr) {
    uint64_t l2_pa = aarch64_vm_next_table(g_build_root_pa, va, 1);
    uint64_t l3_pa;
    uint64_t* l3;
    if (!l2_pa) return;
    l3_pa = aarch64_vm_next_table(l2_pa, va, 2);
    if (!l3_pa) return;
    l3 = aarch64_vm_table_ptr(l3_pa);
    l3[aarch64_vm_index(va, 3)] = (pa & AARCH64_PTE_ADDR_MASK) | attr | AARCH64_PTE_VALID | AARCH64_PTE_TABLE;
}

/* 2MB ブロックを 1 つ張る。L2 の descriptor はブロックなので AARCH64_PTE_TABLE を
 * 立てない (0b01)。ここを 0b11 にするとテーブルとして辿られて壊れる */
static void aarch64_vm_map_block(uint64_t va, uint64_t pa, uint64_t attr) {
    uint64_t l2_pa = aarch64_vm_next_table(g_build_root_pa, va, 1);
    uint64_t* l2;
    if (!l2_pa) return;
    l2 = aarch64_vm_table_ptr(l2_pa);
    l2[aarch64_vm_index(va, 2)] = (pa & AARCH64_PTE_ADDR_MASK) | attr | AARCH64_PTE_VALID;
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

/* **必ず 4KB ページで張る。** 後から同じ範囲に細かい権限を重ねる予定の所は
 * こちらを使う。2MB ブロックで張ってしまうと、重ねようとした時点で
 * 「ブロックの上にテーブルを作れない」衝突になる (実際に踏んだ) */
static void aarch64_vm_map_pages(uint64_t va, uint64_t pa, uint64_t size, uint64_t attr) {
    uint64_t end = va + size;
    while (va < end && !g_vm_failed) {
        aarch64_vm_map_page(va, pa, attr);
        va += AARCH64_PAGE_SIZE;
        pa += AARCH64_PAGE_SIZE;
    }
}

static uint64_t aarch64_align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t aarch64_align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* テーブルを歩いて VA の物理アドレスを求める。**MMU が実際に何を見るかを
 * 自分で辿り直す**ので、有効化前に「思ったとおりに張れているか」を確認できる */
uint64_t aarch64_vm_translate_in(uint64_t root_pa, uint64_t va) {
    uint64_t* table = aarch64_vm_table_ptr(root_pa);
    for (int level = 1; level <= 3; level++) {
        uint64_t entry = table[aarch64_vm_index(va, level)];
        if ((entry & AARCH64_PTE_VALID) == 0) return 0;
        if (level == 3) return (entry & AARCH64_PTE_ADDR_MASK) | (va & 0xfffULL);
        if ((entry & AARCH64_PTE_TABLE) == 0) {
            /* ブロック。ここで翻訳が終わる */
            uint64_t block_size = (level == 1) ? (1ULL << 30) : AARCH64_BLOCK_SIZE;
            return (entry & AARCH64_PTE_ADDR_MASK) | (va & (block_size - 1));
        }
        table = aarch64_vm_table_ptr(entry & AARCH64_PTE_ADDR_MASK);
    }
    return 0;
}

/* カーネル側 (TTBR1) のテーブルを歩く。呼ぶ側の既定はこちら */
uint64_t aarch64_vm_translate(uint64_t va) {
    return aarch64_vm_translate_in(g_kernel_root_pa, va);
}

#define AARCH64_UART_SIZE   0x00001000ULL

/* ---- TTBR1: カーネルの上位 VA (M3b) --------------------------------------
 *
 * **ここを組み立てているとき、C はまだ物理アドレスで走っている。**
 * したがって __text_start などのシンボルは物理アドレスを返す。
 * 上位 VA は「物理 + AARCH64_KERNEL_VA_OFFSET」で作る。 */
static void aarch64_vm_build_kernel(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t ram_base = b->memory_base;
    uint64_t ram_end  = b->memory_base + b->memory_size;
    uint64_t kend_pa   = aarch64_align_up(aarch64_vm_ptr_pa(__kernel_end), AARCH64_PAGE_SIZE);
    uint64_t kblk_start = aarch64_align_down(aarch64_vm_ptr_pa(__kernel_start), AARCH64_BLOCK_SIZE);
    uint64_t kblk_end   = aarch64_align_up(kend_pa, AARCH64_BLOCK_SIZE);
    uint64_t virtio_size;

    g_kernel_root_pa = aarch64_vm_alloc_table();
    if (!g_kernel_root_pa) return;
    g_build_root_pa = g_kernel_root_pa;

    /* **RAM 全域を上位 VA に張る。** これで phys_to_virt(pa) がそのまま
     * 使えるようになり、DTB 由来の物理アドレスをカーネルから触れる。
     * カーネルが載っている 2MB ブロックだけは後で細かく張り直す */
    if (kblk_start > ram_base) {
        aarch64_vm_map_range(aarch64_phys_to_virt(ram_base), ram_base,
                             kblk_start - ram_base, VM_KERNEL_RW);
    }
    if (kblk_end < ram_end) {
        aarch64_vm_map_range(aarch64_phys_to_virt(kblk_end), kblk_end,
                             ram_end - kblk_end, VM_KERNEL_RW);
    }
    /* カーネルが載っている 2MB ブロックの中は **必ず 4KB ページで**張る。
     * ここに後から区画ごとの権限を重ねるので、ブロックで張ると衝突する。
     * まず .boot (イメージの先頭ページ) を張り、区画を重ね、最後に
     * 残り (ブートスタックが載る) を張る */
    aarch64_vm_map_pages(aarch64_phys_to_virt(kblk_start), kblk_start,
                         aarch64_vm_ptr_pa(__kernel_start) - kblk_start, VM_KERNEL_RW);

    /* 区画ごとに権限を分ける。.text を書き込み可のままにしない /
     * .data を実行可のままにしない。riscv64 の vm_init と同じ考え方。
     * **上の一括マッピングの後に張ること。** 順序を逆にすると権限が戻る */
    aarch64_vm_map_pages(aarch64_phys_to_virt(aarch64_vm_ptr_pa(__text_start)),
                         aarch64_vm_ptr_pa(__text_start),
                         (uint64_t)(__text_end - __text_start), VM_KERNEL_TEXT);
    aarch64_vm_map_pages(aarch64_phys_to_virt(aarch64_vm_ptr_pa(__rodata_start)),
                         aarch64_vm_ptr_pa(__rodata_start),
                         (uint64_t)(__rodata_end - __rodata_start), VM_KERNEL_RO);
    aarch64_vm_map_pages(aarch64_phys_to_virt(aarch64_vm_ptr_pa(__data_start)),
                         aarch64_vm_ptr_pa(__data_start),
                         (uint64_t)(__data_end - __data_start), VM_KERNEL_RW);
    aarch64_vm_map_pages(aarch64_phys_to_virt(aarch64_vm_ptr_pa(__bss_start)),
                         aarch64_vm_ptr_pa(__bss_start),
                         (uint64_t)(__bss_end - __bss_start), VM_KERNEL_RW);
    /* **EL0 のページもカーネル側に張る。** カーネルがイメージを読んで
     * プロセスごとのデータページへ写すので、上位 VA から見えている必要が
     * ある (張り忘れて、空間を作る途中で translation fault になった)。
     * ここでの権限はカーネル用。EL0 に見せるのは TTBR0 側の張り方で決まる */
    aarch64_vm_map_pages(aarch64_phys_to_virt(aarch64_vm_ptr_pa(__user_text_start)),
                         aarch64_vm_ptr_pa(__user_text_start),
                         (uint64_t)(__user_text_end - __user_text_start), VM_KERNEL_RO);
    aarch64_vm_map_pages(aarch64_phys_to_virt(aarch64_vm_ptr_pa(__user_data_start)),
                         aarch64_vm_ptr_pa(__user_data_start),
                         (uint64_t)(__user_data_end - __user_data_start), VM_KERNEL_RW);

    /* ブロックの残り。ブートスタックがここに載る */
    aarch64_vm_map_pages(aarch64_phys_to_virt(kend_pa), kend_pa,
                         kblk_end - kend_pa, VM_KERNEL_RW);

    /* MMIO。**Device 属性で張ること。** Normal で張ると UART への書き込みが
     * キャッシュに溜まって出てこない、GIC の読みが古い値を返す、という形で
     * 壊れる (QEMU では見逃せても実機で出る)。
     *
     * アドレスは DTB 由来 = 物理なので、上位 VA に直して張る。
     * **GIC は Distributor と CPU Interface を別々に張る。** 連続している
     * とは限らない (QEMU virt は隣り合うが、Pi 4 は違う) */
    aarch64_vm_map_range(aarch64_phys_to_virt(b->gicd_base), b->gicd_base,
                         b->gicd_size, VM_DEVICE_RW);
    aarch64_vm_map_range(aarch64_phys_to_virt(b->gicc_base), b->gicc_base,
                         b->gicc_size, VM_DEVICE_RW);
    aarch64_vm_map_range(aarch64_phys_to_virt(b->uart_base), b->uart_base,
                         AARCH64_UART_SIZE, VM_DEVICE_RW);

    /* **virtio が無い機械では張らない。** 番地 0 は「この機械には無い」の印
     * (boot.c)。無い番地を Device で張ると、RAM のブロックと重なった瞬間に
     * block/table conflict でテーブル構築ごと失敗する。
     * 実測: Pi 4 で virt の 0x0a000000 を張ろうとして
     *       "block/table conflict at 0xffffff800a000000"。
     *
     * **枠数は virtio_blk_mmio.c と同じ式で数える。** 片方だけ 32 枠を
     * 見に行くと、張っていない枠を読んで落ちる */
    /* Pi 4 の SD カードコントローラ。**virtio と同じく、番地 0 は
     * 「この機械には無い」の印** (QEMU virt には EMMC2 が無い) */
    if (b->emmc2_base) {
        uint64_t sz = b->emmc2_size ? b->emmc2_size : AARCH64_PAGE_SIZE;
        if (sz < AARCH64_PAGE_SIZE) sz = AARCH64_PAGE_SIZE;
        aarch64_vm_map_range(aarch64_phys_to_virt(b->emmc2_base), b->emmc2_base,
                             sz, VM_DEVICE_RW);
    }

    if (b->first_virtio_mmio_base) {
        uint32_t slots = b->virtio_mmio_count ? b->virtio_mmio_count : 32;
        virtio_size = (uint64_t)slots * b->virtio_mmio_stride;
        if (virtio_size < AARCH64_PAGE_SIZE) virtio_size = AARCH64_PAGE_SIZE;
        aarch64_vm_map_range(aarch64_phys_to_virt(b->first_virtio_mmio_base),
                             b->first_virtio_mmio_base, virtio_size, VM_DEVICE_RW);
    }

    /* ---- PCIe (ECAM と BAR の窓) ------------------------------------------
     *
     * **番地 0 は「この機械には無い」の印** (virtio / emmc2 と同じ)。
     * QEMU virt にはあるが raspi4b には無い。
     *
     * **ECAM 全体は張らない。** 256MB あるが、いま見るのはバス 0 だけ
     * (256 デバイス x 8 関数 x 4KB = 1MB)。
     * **MMIO 窓も全部は張らない** — 750MB あるが BAR を配るのは先頭だけ */
    if (b->pcie_ecam_base) {
        uint64_t ecam_sz = b->pcie_ecam_size;
        if (ecam_sz > AARCH64_PCIE_ECAM_MAP_SIZE) ecam_sz = AARCH64_PCIE_ECAM_MAP_SIZE;
        aarch64_vm_map_range(aarch64_phys_to_virt(b->pcie_ecam_base),
                             b->pcie_ecam_base, ecam_sz, VM_DEVICE_RW);
    }
    if (b->pcie_mmio_base) {
        uint64_t win = b->pcie_mmio_size;
        if (win > AARCH64_PCIE_MMIO_MAP_SIZE) win = AARCH64_PCIE_MMIO_MAP_SIZE;
        aarch64_vm_map_range(aarch64_phys_to_virt(b->pcie_mmio_base),
                             b->pcie_mmio_base, win, VM_DEVICE_RW);
    }

    /* ---- フレームバッファ ------------------------------------------------
     *
     * **専用の VA に張る。HHDM には重ねない** (理由は vm.h の
     * AARCH64_FB_VA_BASE の注記)。要点は 2 つ:
     *
     *   - **RAM の外に返ってくることがある。** QEMU の raspi4b は RAM 末尾
     *     0x3c000000 の上、0x3c100000 に返す。HHDM は RAM しか張らないので
     *     そのままでは届かない (実測で判明)
     *   - RAM の中に返ってきた場合、HHDM が同じ物理を Normal WB で張って
     *     いる。**同じ物理を別の属性で 2 通り張ってはいけない**ので、
     *     どちらにせよ別の VA が要る
     *
     * 属性は Normal NC。Device では遅すぎ、Normal WB では書いた絵が
     * キャッシュに留まって VideoCore から見えない */
    {
        const aarch64_fb_info_t* fb = aarch64_fb_info();
        if (fb->base != 0 && fb->size != 0) {
            uint64_t sz = ((uint64_t)fb->size + AARCH64_PAGE_SIZE - 1) &
                          ~(AARCH64_PAGE_SIZE - 1);
            aarch64_vm_map_range(AARCH64_FB_VA_BASE, fb->base, sz, VM_FB_RW);
        }
    }
}

/* ---- TTBR0: 移行のあいだだけの恒等マッピング ----------------------------
 *
 * **MMU を入れた瞬間、PC も sp もまだ物理を指している。** そこが有効で
 * なければ即座に迷子になる。上位 VA へ飛んだら不要になるので、飛んだ後に
 * ユーザー用のテーブルへ差し替えて捨てる。
 *
 * 実行できる必要があるので PXN を立てない。 */
static void aarch64_vm_build_ident(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t kblk_start = aarch64_align_down(aarch64_vm_ptr_pa(__kernel_start),
                                             AARCH64_BLOCK_SIZE);
    /* カーネルイメージ + ブートスタック (8 CPU x 64KB) が入るだけ */
    uint64_t kblk_end = aarch64_align_up(aarch64_vm_ptr_pa(__kernel_end) + 8 * 65536,
                                         AARCH64_BLOCK_SIZE);

    g_ident_root_pa = aarch64_vm_alloc_table();
    if (!g_ident_root_pa) return;
    g_build_root_pa = g_ident_root_pa;

    aarch64_vm_map_range(kblk_start, kblk_start, kblk_end - kblk_start, VM_IDENT_RWX);
    /* 移行の途中で何か言えるように UART だけ張っておく */
    aarch64_vm_map_range(b->uart_base, b->uart_base, AARCH64_UART_SIZE, VM_IDENT_DEVICE);
}

/* ---- TTBR0: ユーザーのアドレス空間 --------------------------------------
 *
 * **カーネルの配置とは無関係の VA に張る。** カーネルを TTBR1 へ移したことで
 * TTBR0 がまるごと空いた。これが「プロセスごとのアドレス空間」の器になる。
 *
 * ユーザーのコードは PC 相対だけで書いてあるので、どの VA でも動く。
 *
 * **テキストは共有、データは私物。** 実際の fork/exec と同じ形にしてある:
 *   .user_text  イメージの物理ページをそのまま張る (読み取り専用なので共有可)
 *   .user_data  pmm から取ったページにイメージから写して張る (空間ごとに別)
 *
 * データを共有してしまうと、片方のプロセスが書いた値がもう片方から見える。
 * それが起きていないことは usermode.c が実測で確かめる。 */
static void aarch64_vm_copy(uint64_t dst_pa, uint64_t src_pa, uint64_t size) {
    uint8_t* d = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(dst_pa);
    const uint8_t* s = (const uint8_t*)(uintptr_t)aarch64_phys_to_virt(src_pa);
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

/* 新しいユーザーアドレス空間を作る。戻り値は TTBR0 に入れる物理アドレス。
 * **上位 VA へ移った後に呼ぶこと** (pmm のページを phys_to_virt で触るため) */
uint64_t aarch64_vm_create_user_space(void) {
    uint64_t text_pa = aarch64_vm_ptr_pa(__user_text_start);
    uint64_t text_size = (uint64_t)(__user_text_end - __user_text_start);
    uint64_t data_src_pa = aarch64_vm_ptr_pa(__user_data_start);
    uint64_t data_size = (uint64_t)(__user_data_end - __user_data_start);
    uint64_t data_pages = data_size / AARCH64_PAGE_SIZE;
    uint64_t root_pa, data_pa;
    uint64_t saved_root = g_build_root_pa;

    root_pa = aarch64_pmm_alloc(1);
    if (!root_pa) return 0;
    data_pa = aarch64_pmm_alloc(data_pages);
    if (!data_pa) { aarch64_pmm_free(root_pa, 1); return 0; }

    /* イメージの中身を私物のページへ写す。**ここを忘れると、初期値の
     * 入った変数が 0 のまま始まる** */
    aarch64_vm_copy(data_pa, data_src_pa, data_size);

    g_build_root_pa = root_pa;
    g_tables_used++;
    aarch64_vm_map_range(AARCH64_USER_VA_BASE, text_pa, text_size, VM_USER_TEXT);
    aarch64_vm_map_range(AARCH64_USER_VA_BASE + text_size, data_pa, data_size, VM_USER_RW);
    g_build_root_pa = saved_root;

    return root_pa;
}

/* TTBR0 を差し替える。**ASID はまだ使っていない**ので、TLB を全部捨てる。
 * ASID を入れると差し替えのたびに全部捨てずに済む (M3c 以降) */
void aarch64_vm_switch_user_space(uint64_t root_pa) {
    g_user_root_pa = root_pa;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(root_pa));
    __asm__ volatile("isb");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb nsh");
    __asm__ volatile("isb");
}

/* 起動時に 1 つだけ作る器 (恒等を外すときに TTBR0 へ入れるもの)。
 * 上位 VA へ移る前に呼ばれるので、pmm のページを物理で触る */
static void aarch64_vm_build_user(void) {
    uint64_t text_pa = aarch64_vm_ptr_pa(__user_text_start);
    uint64_t text_size = (uint64_t)(__user_text_end - __user_text_start);
    uint64_t data_pa = aarch64_vm_ptr_pa(__user_data_start);
    uint64_t data_size = (uint64_t)(__user_data_end - __user_data_start);

    g_user_root_pa = aarch64_vm_alloc_table();
    if (!g_user_root_pa) return;
    g_build_root_pa = g_user_root_pa;

    aarch64_vm_map_range(AARCH64_USER_VA_BASE, text_pa, text_size, VM_USER_TEXT);
    aarch64_vm_map_range(AARCH64_USER_VA_BASE + text_size, data_pa, data_size, VM_USER_RW);
}

/* ユーザー空間の入口とスタックの VA。usermode.c が使う */
uint64_t aarch64_vm_user_entry_va(void) {
    return AARCH64_USER_VA_BASE + (uint64_t)(aarch64_user_entry - __user_text_start);
}

uint64_t aarch64_vm_user_stack_top_va(void) {
    return AARCH64_USER_VA_BASE + (uint64_t)(__user_text_end - __user_text_start) +
           (uint64_t)(__user_data_end - __user_data_start);
}

/* ---- ユーザーが渡してきたポインタの検査 ---------------------------------
 *
 * **アドレス空間を分けたので、これができるようになった。** M3a では
 * カーネルと同じテーブルだったので「ユーザーのものか」を区別できなかった。
 *
 * 見るのは 3 つ:
 *   1. TTBR0 の範囲 (上位半分でない) こと。**カーネルの VA を渡されない**
 *   2. ページが張られていること
 *   3. AP が EL0 に開いていること (書き込みなら書けること)
 *
 * AP[1] (bit 6) が EL0 からのアクセス可否、AP[2] (bit 7) が読み取り専用。 */
static uint64_t aarch64_vm_leaf(uint64_t root_pa, uint64_t va) {
    uint64_t* table = aarch64_vm_table_ptr(root_pa);
    for (int level = 1; level <= 3; level++) {
        uint64_t entry = table[aarch64_vm_index(va, level)];
        if ((entry & AARCH64_PTE_VALID) == 0) return 0;
        if (level == 3) return entry;
        if ((entry & AARCH64_PTE_TABLE) == 0) return entry;   /* ブロック */
        table = aarch64_vm_table_ptr(entry & AARCH64_PTE_ADDR_MASK);
    }
    return 0;
}

int aarch64_vm_user_range_ok(uint64_t root_pa, uint64_t va, uint64_t len, int write) {
    uint64_t end;

    if (len == 0) return 1;
    if (!root_pa) return 0;

    /* 上位半分 = カーネル。**ユーザーから渡されてはいけない。**
     * 桁あふれもここで弾く */
    end = va + len;
    if (end < va) return 0;
    if ((va >> (AARCH64_VA_BITS - 1)) != 0 || (end >> (AARCH64_VA_BITS - 1)) != 0) return 0;

    for (uint64_t p = va & ~(AARCH64_PAGE_SIZE - 1ULL); p < end; p += AARCH64_PAGE_SIZE) {
        uint64_t entry = aarch64_vm_leaf(root_pa, p);
        if (!entry) return 0;
        if ((entry & (1ULL << 6)) == 0) return 0;              /* EL0 から触れない */
        if (write && (entry & (1ULL << 7)) != 0) return 0;     /* 読み取り専用 */
    }
    return 1;
}

uint64_t aarch64_vm_user_root_pa(void) { return g_user_root_pa; }

/* task.c が 0 番タスクの初期値に使う */
uint64_t aarch64_vm_user_root_pa_current(void) { return g_user_root_pa; }
uint64_t aarch64_vm_kernel_root_pa(void) { return g_kernel_root_pa; }

/* ==========================================================================
 * 共有層から見たアドレス空間の操作 (M3c-2a)
 *
 * 上の aarch64_vm_* は「起動時に自分で組み立てる」ための道具で、root は
 * g_build_root_pa という単一のグローバルで持っていた。共有層は
 * **どのアドレス空間か**を毎回渡してくるので、その形に合わせる。
 *
 * root を引数で受けて g_build_root_pa を差し替えるだけの薄い層にしてある。
 * **単一 CPU 前提。** SMP では g_build_root_pa 自体がおかしくなるので、
 * root を引数で持ち回す形に直すこと。
 * ========================================================================== */

/* g_build_root_pa を借りるための入れ物。**必ず戻すこと** */
#define WITH_ROOT(root, body) do {              \
        uint64_t saved__ = g_build_root_pa;     \
        g_build_root_pa = (root);               \
        body;                                   \
        g_build_root_pa = saved__;              \
    } while (0)

void aarch64_vm_activate_address_space(uint64_t root_pa) {
    aarch64_vm_switch_user_space(root_pa);
}

arch_address_space_t arch_vm_kernel_address_space(void) {
    return g_kernel_root_pa;
}

/* **空のテーブルを 1 枚返すだけ。** riscv64 / x86 はここでカーネル領域の
 * 写しを入れているが、AArch64 はカーネルが TTBR1 に居るので要らない。
 * これが「TTBR が 2 本ある」ことの実際の利得 (日報2026-08-09 追2-5) */
arch_address_space_t arch_vm_create_user_address_space(void) {
    uint64_t root_pa = aarch64_pmm_alloc(1);
    if (!root_pa) return 0;
    g_tables_used++;
    return root_pa;
}

/* ページの中身を 1 枚写す。
 *
 * **物理アドレスのまま触らない。** 恒等マッピングを外した後は上位 VA を
 * 通さないと翻訳できない (P2 で kernel/linux_syscall.c が同じ形で落ちた:
 * ESR=0x96000045 / FAR に物理アドレスがそのまま出る)。
 * aarch64_vm_table_ptr が MMU の状態に応じて変換してくれる */
static void aarch64_vm_copy_page(uint64_t dst_pa, uint64_t src_pa) {
    uint64_t* d = aarch64_vm_table_ptr(dst_pa);
    const uint64_t* s = aarch64_vm_table_ptr(src_pa);
    for (uint64_t i = 0; i < AARCH64_PAGE_SIZE / sizeof(uint64_t); i++) d[i] = s[i];
}

/* fork 用にテーブルを 1 段ぶん写す。level は aarch64_vm_index と同じ 1..3。
 *
 * **カーネル領域を避ける処理は要らない。** riscv64 / x86 は 1 本のテーブルに
 * カーネルとユーザーが同居するので「カーネルと同じエントリなら共有する」
 * 判定が要る (kernel/riscv64/vm.c の riscv64_vm_clone_table)。AArch64 は
 * カーネルが TTBR1 に居るので、TTBR0 の中身はすべてユーザーのもの。
 *
 * ページは**その場で写す (eager copy)。** CoW はまだ無いので、fork した
 * 瞬間に親と同じだけの物理ページを食う。 */
static int aarch64_vm_clone_table(uint64_t dst_pa, uint64_t src_pa, int level) {
    uint64_t* dst = aarch64_vm_table_ptr(dst_pa);
    const uint64_t* src = aarch64_vm_table_ptr(src_pa);

    for (uint64_t i = 0; i < AARCH64_PTES; i++) {
        uint64_t entry = src[i];
        dst[i] = 0;
        if ((entry & AARCH64_PTE_VALID) == 0) continue;

        if (level < 3) {
            uint64_t child;
            /* L1/L2 で TABLE ビットが落ちていればブロック写像。ユーザー空間に
             * ブロックは張っていないので、**黙って共有せず失敗させる** —
             * 共有すると親子で同じ物理ページを書き合う */
            if ((entry & AARCH64_PTE_TABLE) == 0) {
                aarch64_uart_puts("  vm: clone: 想定外のブロック写像\n");
                return -1;
            }
            child = aarch64_vm_alloc_table();
            if (!child) return -1;
            dst[i] = (child & AARCH64_PTE_ADDR_MASK) | AARCH64_PTE_VALID | AARCH64_PTE_TABLE;
            if (aarch64_vm_clone_table(child, entry & AARCH64_PTE_ADDR_MASK, level + 1) < 0) {
                return -1;
            }
            continue;
        }

        /* L3 = ページ本体。**属性はそのまま、物理アドレスだけ差し替える。**
         *
         * **pmm_alloc を使う (生の aarch64_pmm_alloc ではない)。**
         * ユーザーページは参照カウントで管理されていて、解放側の pmm_free は
         * カウントが 0 より大きいときしか減らさない。生で取ると refcount が
         * 0 のままになり、arch_vm_destroy_user_address_space が呼ばれても
         * **そのページだけ永久に返らない** */
        {
            uint64_t new_pa = (uint64_t)(uintptr_t)pmm_alloc(1);
            if (!new_pa) return -1;
            aarch64_vm_copy_page(new_pa, entry & AARCH64_PTE_ADDR_MASK);
            dst[i] = (new_pa & AARCH64_PTE_ADDR_MASK) | (entry & ~AARCH64_PTE_ADDR_MASK);
        }
    }
    return 0;
}

/* fork 用。親のアドレス空間を丸ごと写した新しい空間を返す。失敗なら 0。
 * **失敗したら途中まで作ったものを捨てる** — 半端な空間を返すと、子が
 * 穴の空いた空間で走り出して原因不明のアボートになる */
arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space) {
    uint64_t root_pa;

    if (!address_space) return 0;
    root_pa = (uint64_t)arch_vm_create_user_address_space();
    if (!root_pa) return 0;

    /* ルートは L1 (VA 39bit / 4KB granule なので L1 が最上位) */
    if (aarch64_vm_clone_table(root_pa, (uint64_t)address_space, 1) < 0) {
        arch_vm_destroy_user_address_space(root_pa);
        return 0;
    }
    return (arch_address_space_t)root_pa;
}

/* アドレス空間を丸ごと返す (P3-4)。level は aarch64_vm_index と同じ 1..3。
 *
 * **確保したときと同じ流儀で返すこと。**
 *
 *   L3 のページ本体  pmm_alloc で取った -> pmm_free (参照カウントを 1 つ減らす)
 *   L1/L2 のテーブル  aarch64_pmm_alloc で取った -> aarch64_pmm_free (生)
 *
 * 取り違えると、**片方は永久に返らず、もう片方はまだ使われているページを
 * 配り直す。** どちらも「その場では動く」ので気づきにくい。
 *
 * カーネル領域を避ける処理は要らない (TTBR0 の中身はすべてユーザーのもの)。 */
static void aarch64_vm_destroy_table(uint64_t table_pa, int level) {
    uint64_t* table = aarch64_vm_table_ptr(table_pa);

    for (uint64_t i = 0; i < AARCH64_PTES; i++) {
        uint64_t entry = table[i];
        if ((entry & AARCH64_PTE_VALID) == 0) continue;

        if (level < 3) {
            /* ブロック写像は張っていない。**黙って素通りせず**、
             * 万一あったら気づけるようにする (あれば漏れる) */
            if (entry & AARCH64_PTE_TABLE) {
                aarch64_vm_destroy_table(entry & AARCH64_PTE_ADDR_MASK, level + 1);
            } else {
                aarch64_uart_puts("  vm: destroy: 想定外のブロック写像\n");
            }
        } else {
            pmm_free((void*)(uintptr_t)(entry & AARCH64_PTE_ADDR_MASK), 1);
        }
        table[i] = 0;
    }
    aarch64_pmm_free(table_pa, 1);
}

void arch_vm_destroy_user_address_space(arch_address_space_t address_space) {
    if (!address_space) return;
    /* ルートは L1 (VA 39bit / 4KB granule) */
    aarch64_vm_destroy_table((uint64_t)address_space, 1);
}

void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr,
                      uint64_t paddr, uint64_t flags) {
    if (!address_space) return;
    WITH_ROOT(address_space, aarch64_vm_map_page(vaddr, paddr, flags));
}

/* **必ず 4KB ページで張る。** 共有層が張るのはユーザーのページで、
 * 後から権限を重ねられる必要がある。2MB ブロックにすると
 * 「ブロックの上にテーブルを作れない」衝突になる (日報2026-08-09 追2-5) */
void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr,
                       uint64_t paddr, uint64_t size, uint64_t flags) {
    if (!address_space) return;
    WITH_ROOT(address_space, aarch64_vm_map_pages(vaddr, paddr, size, flags));
}

uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr) {
    if (!address_space) return 0;
    return aarch64_vm_translate_in(address_space, vaddr);
}

/* L3 の descriptor を無効にする。**TLB を捨てるところまでやる。**
 * 捨てないと、外したはずのページが読めたままになる */
void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr) {
    uint64_t l2_pa, l3_pa;
    uint64_t* l3;

    if (!address_space) return;
    l2_pa = aarch64_vm_walk_existing(address_space, vaddr, 1);
    if (!l2_pa) return;
    l3_pa = aarch64_vm_walk_existing(l2_pa, vaddr, 2);
    if (!l3_pa) return;
    l3 = aarch64_vm_table_ptr(l3_pa);
    l3[aarch64_vm_index(vaddr, 3)] = 0;
    aarch64_vm_flush_va(vaddr);
}

/* 属性だけ差し替える。物理アドレスはそのまま */
void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr,
                               uint64_t flags) {
    uint64_t l2_pa, l3_pa, entry;
    uint64_t* l3;
    uint64_t index;

    if (!address_space) return;
    l2_pa = aarch64_vm_walk_existing(address_space, vaddr, 1);
    if (!l2_pa) return;
    l3_pa = aarch64_vm_walk_existing(l2_pa, vaddr, 2);
    if (!l3_pa) return;
    l3 = aarch64_vm_table_ptr(l3_pa);
    index = aarch64_vm_index(vaddr, 3);
    entry = l3[index];
    if ((entry & AARCH64_PTE_VALID) == 0) return;
    l3[index] = (entry & AARCH64_PTE_ADDR_MASK) | flags | AARCH64_PTE_VALID | AARCH64_PTE_TABLE;
    aarch64_vm_flush_va(vaddr);
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
    /* **TTBR0 = 恒等 (移行用) / TTBR1 = カーネルの上位 VA。**
     * TTBR0 が無いと MMU を入れた瞬間に PC が迷子になり、
     * TTBR1 が無いと上位 VA へ飛べない。両方要る */
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(g_ident_root_pa));
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"(g_kernel_root_pa));
    __asm__ volatile("isb");

    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb nsh");
    __asm__ volatile("isb");

    /* 読んで OR せず、決めた値をそのまま書く (上の SCTLR_EL1_VALUE を参照) */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"((uint64_t)SCTLR_EL1_VALUE));
    __asm__ volatile("isb");
}

uint64_t aarch64_read_sctlr(void) {
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
volatile int g_aarch64_vm_expect_fault;
volatile uint64_t g_aarch64_vm_fault_esr;

/* 探針に使う VA を実行時に選ぶ。
 *
 * **固定値 (0x20000000 = 「RAM の手前・デバイスの奥」) にしていたが、
 * RAM が物理 0 から始まる機械では破綻した。** Pi 4 がまさにそれ。
 * 2 つの理由が重なっている:
 *
 *   - HHDM が RAM 全体を張るので、その番地は**実際に張られている**
 *   - **低位 VA を aarch64_vm_translate に渡してはいけない。** あれは
 *     TTBR1 のテーブルを歩き、索引は VA の上位ビットを落とす。
 *     0x20000000 は **HHDM の先頭 1GB** を索いてしまう
 *
 * 実機で `SKIP (探針アドレスが張られている)` になり、検査の網が 1 つ
 * 落ちていた (日報2026-08-16 §7)。QEMU virt は RAM が 0x40000000 から
 * なので、たまたま当たっていただけ。
 *
 * **HHDM の外側 (RAM の終わりより後ろ) の上位 VA から探す。** そこは
 * 上位 VA なので translate の索引も一致し、判定と実アクセスが同じ所を見る。
 * 見つからなければ 0 を返す */
static uint64_t aarch64_vm_pick_probe_va(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t past_ram = b ? (b->memory_base + b->memory_size) : 0;

    /* RAM の終わりから 1GB 空ける。端の切り上げで踏まないため */
    past_ram = aarch64_align_up(past_ram + (1ULL << 30), 1ULL << 30);

    for (int i = 0; i < 64; i++) {
        uint64_t va = AARCH64_KERNEL_VA_OFFSET + past_ram + (uint64_t)i * (1ULL << 30);
        if (aarch64_vm_translate(va) == 0) return va;
    }
    return 0;
}

void aarch64_vm_fault_probe(void) {
    uint64_t probe_va = aarch64_vm_pick_probe_va();
    volatile uint32_t* p;
    uint64_t esr, ec, dfsc;

    if (probe_va == 0) {
        aarch64_uart_puts("  mmu probe : SKIP (未マップの VA が見つからない)\n");
        return;
    }
    p = (volatile uint32_t*)(uintptr_t)probe_va;

    /* 先に「張られていない」ことをテーブル側で確かめる。ここが 0 でなければ
     * 探針の前提が崩れているので、fault が上がらなくても当然になる */
    if (aarch64_vm_translate(probe_va) != 0) {
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

/* 上位 VA へ飛ぶ (entry.S)。**戻ってこない。**
 *   x0 = 新しい sp (上位 VA)、x1 = 飛び先 (上位 VA) */
uint64_t aarch64_pmm_total(void);
uint64_t aarch64_pmm_used(void);

void aarch64_vm_enter_high(uint64_t new_sp, uint64_t cont);

/* リンカスクリプトの KERNEL_VA_OFFSET (entry.S) */
uint64_t aarch64_link_va_offset(void);

/* 上位 VA に着いてから続きをやる。boot.c にある */
void aarch64_boot_continue(void);

/* 恒等マッピングを捨てて、TTBR0 をユーザーのテーブルに差し替える。
 * **これを呼んだ後、物理アドレスでは何も触れなくなる。** */
void aarch64_vm_drop_identity(void) {
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(g_user_root_pa));
    __asm__ volatile("isb");
    __asm__ volatile("tlbi vmalle1");
    __asm__ volatile("dsb nsh");
    __asm__ volatile("isb");
}

/* MMU を入れて上位 VA へ移る。**戻ってこない。**
 *
 * ここは C がまだ物理アドレスで走っている。シンボルのアドレスを取ると
 * 物理が返るので、上位 VA は phys_to_virt で作る。 */
void aarch64_vm_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t kstart_pa = aarch64_vm_ptr_pa(__kernel_start);
    uint64_t new_sp, cont;

    aarch64_uart_puts("--- M2/M3b: MMU (4KB granule, VA 39bit, TTBR1 = カーネル) ---\n");

    /* **リンカスクリプトとヘッダで VA のずらし幅が一致しているか。**
     *
     * ただし実測すると、**ヘッダ側だけずらした場合はここに来る前に死ぬ。**
     * start.S も同じヘッダの値を使って __bss_start や aarch64_early_main の
     * 物理アドレスを計算しているので、C に入る前に迷子になり、起動バナー
     * すら出ない (逆確認で確認済み)。
     *
     * それでも残してあるのは、**vm.c だけが別の値を持ってしまった場合**
     * (この関数の中で計算をいじった、など) を捕まえられるため。
     * 起動バナーが出ないときは、ここではなく start.S の計算を疑うこと。 */
    if (aarch64_link_va_offset() != AARCH64_KERNEL_VA_OFFSET) {
        aarch64_uart_puts("  KERNEL_VA_OFFSET がリンカスクリプトと違う: ld=");
        aarch64_uart_puthex64(aarch64_link_va_offset());
        aarch64_uart_puts(" hdr=");
        aarch64_uart_puthex64(AARCH64_KERNEL_VA_OFFSET);
        aarch64_uart_puts("\naarch64-mmu-BAD\n");
        return;
    }

    aarch64_vm_build_kernel();     /* TTBR1: カーネルの上位 VA */
    aarch64_vm_build_ident();      /* TTBR0: 移行のあいだだけの恒等 */
    aarch64_vm_build_user();       /* TTBR0: 上位へ飛んだ後のユーザー空間 */
    if (g_vm_failed) {
        aarch64_uart_puts("aarch64-mmu-BAD (テーブル構築に失敗)\n");
        return;
    }

    aarch64_uart_puts("  tables    : ");
    aarch64_uart_puthex64(g_tables_used);
    aarch64_uart_puts("  (pmm から確保。空き ");
    aarch64_uart_puthex64(aarch64_pmm_total() - aarch64_pmm_used());
    aarch64_uart_puts(" ページ)\n  kernel VA : ");
    aarch64_uart_puthex64(aarch64_phys_to_virt(kstart_pa));
    aarch64_uart_puts("  (物理 ");
    aarch64_uart_puthex64(kstart_pa);
    aarch64_uart_puts(")\n");

    /* 有効化の前に、自分でテーブルを歩いて要るものが張れているかを見る。
     * **上位 VA 側と恒等側の両方を見る。** どちらが欠けても沈黙する:
     *   恒等が欠ける → MMU を入れた瞬間に PC が迷子
     *   上位が欠ける → 飛んだ瞬間に迷子 */
    aarch64_uart_puts("  ttbr1 text: ");
    aarch64_uart_puthex64(aarch64_vm_translate_in(g_kernel_root_pa,
                          aarch64_phys_to_virt(aarch64_vm_ptr_pa(__text_start))));
    aarch64_uart_puts("\n  ttbr1 uart: ");
    aarch64_uart_puthex64(aarch64_vm_translate_in(g_kernel_root_pa,
                          aarch64_phys_to_virt(b->uart_base)));
    aarch64_uart_puts("\n  ttbr1 gicc: ");
    aarch64_uart_puthex64(aarch64_vm_translate_in(g_kernel_root_pa,
                          aarch64_phys_to_virt(b->gicc_base)));
    aarch64_uart_puts("\n  ttbr1 ram : ");
    aarch64_uart_puthex64(aarch64_vm_translate_in(g_kernel_root_pa,
                          aarch64_phys_to_virt(b->memory_base + b->memory_size - 1)));
    aarch64_uart_puts("\n  ttbr0 iden: ");
    aarch64_uart_puthex64(aarch64_vm_translate_in(g_ident_root_pa, kstart_pa));
    /* **ユーザー空間の入口が、イメージの .user_text を指していること。**
     * 値そのものはコードの大きさで動くので、期待値を書かずにここで照合する
     * (スモークに物理アドレスを直書きすると、行が増えるたびに落ちる) */
    aarch64_uart_puts("\n  ttbr0 user: ");
    aarch64_uart_puthex64(aarch64_vm_translate_in(g_user_root_pa, AARCH64_USER_VA_BASE));
    aarch64_uart_puts(aarch64_vm_translate_in(g_user_root_pa, AARCH64_USER_VA_BASE) ==
                      aarch64_vm_ptr_pa(__user_text_start)
                      ? "  ok (.user_text を指している)\n" : "  BAD\n");

    if (aarch64_vm_translate_in(g_kernel_root_pa,
            aarch64_phys_to_virt(aarch64_vm_ptr_pa(__text_start))) == 0 ||
        aarch64_vm_translate_in(g_kernel_root_pa, aarch64_phys_to_virt(b->uart_base)) == 0 ||
        aarch64_vm_translate_in(g_kernel_root_pa, aarch64_phys_to_virt(b->gicc_base)) == 0 ||
        aarch64_vm_translate_in(g_kernel_root_pa,
            aarch64_phys_to_virt(b->memory_base + b->memory_size - 1)) == 0 ||
        aarch64_vm_translate_in(g_ident_root_pa, kstart_pa) == 0 ||
        aarch64_vm_translate_in(g_user_root_pa, AARCH64_USER_VA_BASE) == 0) {
        aarch64_uart_puts("aarch64-mmu-BAD (有効化前の確認で 0 が出た)\n");
        return;
    }

    /* 移行のあいだは割り込みを閉じる。ベクタはまだ物理を指しているので、
     * 上位へ飛んで VBAR を張り替えるまで例外を受けない */
    __asm__ volatile("msr daifset, #2");
    aarch64_mmu_enable();

    /* ここに文字が出れば「MMU on のまま、恒等マッピングで UART に届いている」 */
    aarch64_uart_puts("  mmu on    : ok (まだ物理アドレスで走っている)\n");

    /* **新しいスタックを上位 VA に用意して飛ぶ。**
     * いまのスタックには物理アドレスの戻り先が積まれているので、そのまま
     * 使い続けると恒等マッピングを外した瞬間に破綻する。
     * CPU 0 のブートスタックの頂点 (カーネルイメージ直後 + 64KB) を使う */
    new_sp = aarch64_phys_to_virt(
        aarch64_align_up(aarch64_vm_ptr_pa(__kernel_end), AARCH64_PAGE_SIZE) + 65536);
    cont = aarch64_phys_to_virt((uint64_t)(uintptr_t)aarch64_boot_continue);

    aarch64_vm_enter_high(new_sp, cont);   /* 戻ってこない */
}

uint64_t aarch64_vm_root_pa(void) {
    return g_kernel_root_pa;
}


/* ---- フレームバッファをユーザー空間に見せる (DOOM 用) --------------------
 *
 * **カーネルの写像 (AARCH64_FB_VA_BASE) とは別に張る。**あちらは TTBR1 で
 * EL0 からは触れない。同じ物理を TTBR0 側にも、同じ属性 (Normal NC) で張る。
 *
 * 返すのはユーザーから見た VA。0 なら失敗 */
uint64_t aarch64_vm_map_fb_user(arch_address_space_t as, uint64_t uva) {
    const aarch64_fb_info_t* fb = aarch64_fb_info();
    uint64_t size;
    if (!as || !fb || fb->base == 0 || fb->size == 0) return 0;
    size = ((uint64_t)fb->size + AARCH64_PAGE_SIZE - 1) & ~(AARCH64_PAGE_SIZE - 1);
    /* **4KB ページで張る** (arch_vm_map_range と同じ道)。ユーザーの
     * ページテーブルに 2MB ブロックを混ぜると、後から権限を重ねるときに
     * 衝突する (日報2026-08-09 追2-5) */
    WITH_ROOT(as, aarch64_vm_map_pages(uva, fb->base, size, VM_USER_FB));
    return uva;
}
