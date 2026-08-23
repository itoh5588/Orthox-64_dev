#ifndef ORTHOX_ARCH_AARCH64_VM_H
#define ORTHOX_ARCH_AARCH64_VM_H

/* .S からも読むので、**アセンブラが解釈できない書き方をしない**。
 * C 専用の宣言は下の #ifndef __ASSEMBLER__ の中に置くこと。 */

/* カーネルの VA と物理アドレスの差 (M3b)。
 *
 * TCR_EL1.T1SZ = 25 (VA 39bit) なので、TTBR1 が受け持つのは VA[63:39] が
 * 全部 1 の範囲 = 0xffffff8000000000 以上。カーネルの VA は「物理 + この値」。
 *
 * **scripts/kernel-aarch64.ld の KERNEL_VA_OFFSET と同じ値にすること。**
 * ずれると、MMU を入れて上位 VA へ飛んだ瞬間に沈黙する。 */
#define AARCH64_KERNEL_VA_OFFSET 0xffffff8000000000

/* EL0 のコードを置く VA。**カーネルの配置とは無関係に決める。**
 * TTBR0 は M3b からユーザー専用なので、下位側のどこでもよい。
 * ユーザーのコードは PC 相対だけで書いてあるので、どの VA でも動く */
#define AARCH64_USER_VA_BASE 0x0000000000400000

#ifndef __ASSEMBLER__

#include <stdint.h>

/* 物理 <-> カーネル VA。**恒等マッピングが外れた後は必ずこれを通す。**
 *
 * MMU を入れる前は PC が物理なので、C からシンボルのアドレスを取ると
 * 物理が返る。上位 VA へ飛んだ後は同じ式が VA を返す。
 * 「いまどちらの世界に居るか」で意味が変わるので、
 * **DTB 由来のアドレス (常に物理) は必ず phys_to_virt を通すこと**。 */
static inline uint64_t aarch64_phys_to_virt(uint64_t pa) {
    return pa + AARCH64_KERNEL_VA_OFFSET;
}

static inline uint64_t aarch64_virt_to_phys(uint64_t va) {
    return va - AARCH64_KERNEL_VA_OFFSET;
}

/* ---- descriptor のビット (M3c-2a でここへ移した) -------------------------
 *
 * kernel/aarch64/vm.c にあったが、共有層向けの arch_vm_* が同じビットを
 * 組み立てるので**出どころを 1 か所にする**。riscv64 も
 * include/riscv64/vm.h にビットを置いている。
 *
 * bit[1:0] が形を決める。ここが AArch64 の踏みやすいところ:
 *
 *   L1 / L2   0b01 = ブロック (そこで翻訳を終える) / 0b11 = 次の段のテーブル
 *   L3        **0b11 のみ有効**。0b01 は invalid 扱いになる
 *
 * つまり「テーブル」と「L3 のページ」が同じ 0b11 で、「ブロック」だけが 0b01。 */
#define AARCH64_PTE_VALID       (1ULL << 0)
#define AARCH64_PTE_TABLE       (1ULL << 1)   /* L1/L2 では次段テーブル、L3 ではページ */
#define AARCH64_PTE_ATTRINDX(n) (((uint64_t)(n)) << 2)
#define AARCH64_PTE_AP_RW_EL1   (0ULL << 6)   /* AP[2:1]=00 EL1 で読み書き、EL0 は不可 */
#define AARCH64_PTE_AP_RW_EL0   (1ULL << 6)   /* AP=01 EL1/EL0 とも読み書き */
#define AARCH64_PTE_AP_RO_EL1   (2ULL << 6)   /* AP=10 EL1 で読み取りのみ、EL0 は不可 */
#define AARCH64_PTE_AP_RO_EL0   (3ULL << 6)   /* AP=11 EL1/EL0 とも読み取りのみ */
#define AARCH64_PTE_SH_INNER    (3ULL << 8)   /* Inner Shareable。Normal メモリに付ける */
#define AARCH64_PTE_AF          (1ULL << 10)  /* Access Flag。**必須** */
#define AARCH64_PTE_nG          (1ULL << 11)  /* 0 = global。カーネル領域は 0 のまま */
#define AARCH64_PTE_PXN         (1ULL << 53)  /* EL1 からの実行禁止 */
#define AARCH64_PTE_UXN         (1ULL << 54)  /* EL0 からの実行禁止 */

#define AARCH64_PTE_ADDR_MASK   0x0000fffffffff000ULL

/* MAIR_EL1 の枠。0 番 = Device-nGnRnE / 1 番 = Normal WB */
#define MAIR_IDX_DEVICE 0
#define MAIR_IDX_NORMAL 1
/* Normal Non-Cacheable。**フレームバッファ用。**
 *
 * Device にすると 1 語ずつの厳密な順序になり、画面の書き換えが極端に遅い。
 * かといって Normal WB (キャッシュ有効) にすると、書いた絵がキャッシュに
 * 留まって **VideoCore からは古いまま見える**。間を取って NC にする。
 * 属性値 0x44 = outer NC / inner NC */
#define MAIR_IDX_NORMAL_NC 2

/* フレームバッファを張る上位 VA。
 *
 * **HHDM に重ねない。** 理由が 2 つある:
 *   - フレームバッファは **RAM の外に置かれることがある** (QEMU の raspi4b は
 *     RAM 末尾 0x3c000000 の上、0x3c100000 に返す)。HHDM は RAM しか張らない
 *   - RAM の中に返ってきた場合、HHDM は同じ番地を Normal WB で張っており、
 *     属性が食い違う。**同じ物理を別の属性で 2 通り張るのは禁じ手**
 *
 * TTBR1 の窓 (39bit VA = 512GB) の中で、HHDM から 256GB 離した所に置く。
 * MMU の探針が未マップの VA を探す範囲 (RAM 末尾から 64GB) とも被らない */
#define AARCH64_FB_VA_BASE 0xffffffc000000000ULL

/* PCIe を張る範囲。**全部は張らない。**
 *   ECAM  256MB あるが、いま見るのはバス 0 だけ = 1MB
 *   MMIO  750MB あるが、BAR を配るのは先頭だけ
 * 足りなくなったら増やす。**張っていない所を触ると translation fault で
 * 落ちる**ので、黙って壊れることはない */
/* フレームバッファの写像に使う属性。**MMU を入れた後から張る経路
 * (fb_pci.c) でも同じものを使う**ので、vm.c の中だけに閉じない。
 * Normal NC / EL1 の読み書き / 実行不可 */
#define AARCH64_VM_FB_FLAGS (AARCH64_PTE_ATTRINDX(MAIR_IDX_NORMAL_NC) | \
                             AARCH64_PTE_AF | AARCH64_PTE_AP_RW_EL1 | \
                             AARCH64_PTE_UXN | AARCH64_PTE_PXN)

/* **バス 1 台につき 1MB。** 16MB = バス 0..15。
 *
 * 1MB (バス 0 だけ) にしていたら、**ブリッジの先のバス 1 を読んだ瞬間に
 * translation fault で落ちた** (実測)。Raspberry Pi 4 の実機では USB が
 * バス 1 にいるので、ここが足りないと実機でも同じ所で落ちる */
#define AARCH64_PCIE_ECAM_MAP_SIZE 0x01000000ULL   /* 16MB = バス 0..15 */
/* **64MB。** 16MB では足りなかった — 表示装置のフレームバッファの BAR が
 * 16MB あり、境界を揃えると次のデバイスが窓の外に出る
 * (実測: bochs-display の bar0 が 0x11000000、xHCI が 0x12004000)。
 * 張っていない所を触れば translation fault で落ちるので、足りなければ
 * すぐ分かる */
#define AARCH64_PCIE_MMIO_MAP_SIZE 0x04000000ULL   /* 64MB */

/* Raspberry Pi 4 の外向き窓のうち張る量。**全部 (1GB) は要らない** —
 * xHCI の BAR は 4KB。1MB あれば足りる */
#define AARCH64_PCIE_BRCM_WIN_MAP_SIZE 0x00100000ULL   /* 1MB */

/* EL0 から使えるページの属性。共有層の arch_vm_user_page_flags がこれを返す。
 *
 * **PXN は常に立てる。** EL1 がユーザーのコードを実行できてしまうと、
 * ユーザーが書いた命令をカーネル権限で走らせる道ができる。UXN と PXN は
 * 別のビットで、片方だけでは塞げない。
 *
 * **nG も常に立てる。** ユーザーのページはアドレス空間ごとに違うので、
 * global にすると TTBR0 を差し替えても TLB に残る */
static inline uint64_t aarch64_vm_user_page_attr(int writable, int executable) {
    uint64_t attr = AARCH64_PTE_ATTRINDX(MAIR_IDX_NORMAL) | AARCH64_PTE_SH_INNER | AARCH64_PTE_AF |
                    AARCH64_PTE_nG | AARCH64_PTE_PXN;
    attr |= writable ? AARCH64_PTE_AP_RW_EL0 : AARCH64_PTE_AP_RO_EL0;
    if (!executable) attr |= AARCH64_PTE_UXN;
    return attr;
}

/* ---- 命令キャッシュの同期 (S-3) ------------------------------------------
 *
 * **ユーザーの text を書いたら必ず通す。** exec がセグメントを写した後と、
 * fork がページを写した後の 2 か所。
 *
 * Cortex-A72 の I-cache は PIPT で、**D-cache をスヌープしない。**
 * カーネルは HHDM (Normal WB) 経由の memcpy で text を書くので、
 *   - 書いたバイトが D-cache に残ったまま命令フェッチが走る
 *   - そのページを前に使っていたプログラムの古い I-cache 行を拾う
 * のどちらも起きる。**QEMU は書き込みで変換キャッシュを捨てるので出ない。**
 *
 * 2026-08-23 の実機で、gcc が作ったバイナリが 20 回中 17 回落ちた。
 * ELR=0x400144 (`ldaxr w10,[x9]`、x9 は 2 命令前に確定した定数 0x420138) に
 * 対して FAR=0x4c14f0 が出ており、**ファイルにある命令では成立しない**組。
 * CPU がファイルと違うバイトを実行していた。
 *
 * 手順は ARM ARM の "Synchronization of data and instruction caches" どおり:
 *   dc cvau (PoU まで clean) -> dsb ish -> ic ivau -> dsb ish -> isb
 *
 * **行の大きさは CTR_EL0 から取る。** D と I で違うことがあるので別々に見る
 * (フィールドは「4 バイト語の数の log2」なので 4 << n)。
 *
 * 渡すのは HHDM の VA でよい。**キャッシュは PIPT** なので、同じ物理を指す
 * どの VA から叩いても同じ行に当たる。
 *
 * `ic ivau` は inner-shareable 全体に届くので、SMP でも他コアの I-cache が
 * 揃う (riscv64 の fence.i はハート単位なので、あちらは IPI が要る) */
static inline void aarch64_sync_icache_range(void* va, uint64_t len) {
    if (len == 0) return;

    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ULL << ((ctr >> 16) & 0xfULL);   /* CTR_EL0.DminLine */
    uint64_t iline = 4ULL << (ctr & 0xfULL);           /* CTR_EL0.IminLine */

    uint64_t start = (uint64_t)(uintptr_t)va;
    uint64_t end   = start + len;

    for (uint64_t p = start & ~(dline - 1); p < end; p += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");

    for (uint64_t p = start & ~(iline - 1); p < end; p += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

uint64_t aarch64_vm_user_root_pa_current(void);

/* ---- 共有層から見たアドレス空間 (M3c-2a) --------------------------------
 *
 * riscv64 の arch_address_space_t が satp に入れる root_pa だったのと同じで、
 * **TTBR0_EL1 に入れる root の物理アドレス**。
 *
 * **AArch64 ではカーネルが TTBR1 に居るので、ユーザー空間を作るときに
 * カーネル領域の写しを入れなくてよい** (riscv64 / x86 は入れている)。
 * arch_vm_create_user_address_space が空のテーブルを返すのはそのため。 */
typedef uint64_t arch_address_space_t;

arch_address_space_t arch_vm_kernel_address_space(void);
arch_address_space_t arch_vm_create_user_address_space(void);
arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space);
void arch_vm_destroy_user_address_space(arch_address_space_t address_space);
void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags);
uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr);
void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr);
void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t flags);

void aarch64_vm_activate_address_space(uint64_t root_pa);
uint64_t aarch64_vm_kernel_root_pa(void);

/* MMU が有効か (SCTLR_EL1.M)。
 *
 * **「上位 VA で走っているか」とは別物。** TTBR1 は MMU を入れた時点で
 * 効いているので、まだ恒等マッピングで走っていても上位 VA には届く。
 *
 * **TTBR1 にしか写像が無いもの (フレームバッファ) は、こちらで判断する。**
 * running_high で判断すると、MMU を入れてから高位 VA へ移るまでの間に
 * 物理番地を触ってしまい、恒等マッピングに無いので落ちる
 * (実測: raspi4b で FAR=0x3c110000 の translation fault) */
static inline int aarch64_vm_mmu_enabled(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return (int)(v & 1ULL);
}

/* ---- 非キャッシュ DMA プール ---------------------------------------------
 *
 * **PCIe は CPU のキャッシュとコヒーレントでない機械がある** (Pi 4 がそう。
 * 実機の DTB の pcie ノードに dma-coherent が無い)。xHCI のリングを
 * Normal-WB に置くと、デバイスは古い内容を読み、こちらは古い内容を見る。
 * ここから取ったメモリは HHDM が **Normal-NC** で張ってある。
 *
 * 返るのは **物理番地**。中身は 0 で埋めてある。解放は無い */
uint64_t aarch64_vm_dma_alloc(uint64_t pages);
uint64_t aarch64_vm_dma_pool_base(void);
uint64_t aarch64_vm_dma_pool_bytes(void);

/* いま上位 VA で走っているか。恒等マッピングを外してよいかの判断に使う */
static inline int aarch64_vm_running_high(void) {
    uint64_t pc;
    __asm__ volatile("adr %0, ." : "=r"(pc));
    return (pc >> 39) == 0x1ffffffULL;
}

#endif /* __ASSEMBLER__ */

#endif
