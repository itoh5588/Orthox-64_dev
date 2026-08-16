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

/* いま上位 VA で走っているか。恒等マッピングを外してよいかの判断に使う */
static inline int aarch64_vm_running_high(void) {
    uint64_t pc;
    __asm__ volatile("adr %0, ." : "=r"(pc));
    return (pc >> 39) == 0x1ffffffULL;
}

#endif /* __ASSEMBLER__ */

#endif
