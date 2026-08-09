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

/* いま上位 VA で走っているか。恒等マッピングを外してよいかの判断に使う */
static inline int aarch64_vm_running_high(void) {
    uint64_t pc;
    __asm__ volatile("adr %0, ." : "=r"(pc));
    return (pc >> 39) == 0x1ffffffULL;
}

#endif /* __ASSEMBLER__ */

#endif
