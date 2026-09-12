#ifndef ORTHOX_ARCH_VM_H
#define ORTHOX_ARCH_VM_H

#include <stdint.h>
#include "vmm.h"
#include "pmm.h"

typedef uint64_t arch_address_space_t;

/* arch_vm_* API for x86_64: thin wrappers over the existing vmm_* layer.
 * address_space is a physical PML4 address (CR3 value), matching main's ctx.cr3.
 * Keeping the vmm.c implementation intact preserves main's evolved VM features
 * (CoW, memtrace, COW fault handling) while letting task/syscall code use the
 * arch-agnostic arch_vm_* surface. */

static inline arch_address_space_t arch_vm_kernel_address_space(void) {
    return (arch_address_space_t)vmm_get_kernel_pml4_phys();
}

static inline uint64_t* arch_vm_address_space_root(arch_address_space_t address_space) {
    return (uint64_t*)PHYS_TO_VIRT((void*)(uintptr_t)address_space);
}

static inline arch_address_space_t arch_vm_create_user_address_space(void) {
    /* Allocate a fresh PML4 and copy the kernel half (upper 256 entries) from
     * the kernel PML4 so kernel mappings are inherited by the new user space.
     * The lower 256 entries (user space) are zeroed. */
    void* pml4_phys = pmm_alloc(1);
    if (!pml4_phys) return 0;
    uint64_t* new_root = (uint64_t*)PHYS_TO_VIRT(pml4_phys);
    uint64_t* kernel_root = arch_vm_address_space_root(arch_vm_kernel_address_space());
    for (int i = 0; i < 512; i++) {
        new_root[i] = (i >= 256) ? kernel_root[i] : 0;
    }
    return (arch_address_space_t)(uint64_t)pml4_phys;
}

static inline arch_address_space_t arch_vm_clone_address_space(arch_address_space_t address_space) {
    return (arch_address_space_t)vmm_copy_pml4(arch_vm_address_space_root(address_space));
}

static inline void arch_vm_destroy_user_address_space(arch_address_space_t address_space) {
    vmm_free_user_pml4((uint64_t)address_space);
}

static inline void arch_vm_map_page(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    vmm_map_page(arch_vm_address_space_root(address_space), vaddr, paddr, flags);
}

static inline void arch_vm_map_range(arch_address_space_t address_space, uint64_t vaddr, uint64_t paddr, uint64_t size, uint64_t flags) {
    vmm_map_range(arch_vm_address_space_root(address_space), vaddr, paddr, size, flags);
}

static inline uint64_t arch_vm_get_phys(arch_address_space_t address_space, uint64_t vaddr) {
    return vmm_get_phys(arch_vm_address_space_root(address_space), vaddr);
}

/* **ページの葉を引く。**以下の 3 つが共通で使う。
 * 2MB ページは触らない (mmap / mprotect が作らないので葉だけ見ればよい) */
static inline uint64_t* x86_user_pte(arch_address_space_t address_space, uint64_t vaddr) {
    uint64_t* pml4 = arch_vm_address_space_root(address_space);
    uint64_t* pdp;
    uint64_t* pd;
    uint64_t* pt;

    if (!pml4) return 0;
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return 0;
    pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return 0;
    pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return 0;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return 0;
    pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    return &pt[PT_IDX(vaddr)];
}

/* **このプロセスのページか (2026-09-12 に x86 にも足した)。**
 * 2026-09-11 の時点では「呼び手が無いので足さない」としていたが、
 * mprotect を 3 アーキ共通にしたので呼び手が出来た */
static inline int arch_vm_is_user_page(arch_address_space_t address_space, uint64_t vaddr) {
    uint64_t* pte = x86_user_pte(address_space, vaddr);
    if (!pte) return 0;
    return (*pte & PTE_PRESENT) && (*pte & PTE_USER) ? 1 : 0;
}

/* **保護属性だけ変える。物理アドレスとソフトウェアのビットは残す。**
 *
 * ★ **COW のページを書き込み可にしてはいけない (x86 だけの話)。**
 * x86 の fork は COW (PTE_COW) で、aarch64 / riscv64 は fork の時点で
 * ページを写してしまうので COW が無い。そのため共通版が
 * arch_vm_map_page(as, va, phys, 新しい flags) で貼り直すと、
 * **x86 では COW のページが両プロセスから書けるようになる。**
 * ここを通せばその心配が無い —— 書き込み可にするのは COW でないときだけで、
 * COW のページは読み取り専用のまま残り、書いた瞬間に COW のフォルト処理が
 * 写してから許可する (もとの kernel/x86_64/sys_vm.c の sys_mprotect と同じ)。 */
/* **今の保護属性を読む (2026-09-12)。**mremap が伸ばした分や移した先に
 * 元と同じ保護を引き継ぐのに要る。
 *
 * ★ **COW のページは「書けない」と読めるが、元は書けた。**そのまま読むと
 * mremap した瞬間に書き込み可を失う。PTE_COW が立っていれば writable と
 * 見なす (もとの kernel/x86_64/sys_vm.c の sys_mremap と同じ判断)。 */
static inline int arch_vm_get_page_prot(arch_address_space_t address_space, uint64_t vaddr,
                                        int* writable, int* executable) {
    uint64_t* pte = x86_user_pte(address_space, vaddr);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;
    if (writable) *writable = (*pte & (PTE_WRITABLE | PTE_COW)) != 0;
    if (executable) *executable = (*pte & PTE_NX) == 0;
    return 0;
}

static inline void arch_vm_protect_page(arch_address_space_t address_space, uint64_t vaddr,
                                        int writable, int executable) {
    uint64_t* pte = x86_user_pte(address_space, vaddr);
    if (!pte || !(*pte & PTE_PRESENT)) return;
    *pte &= ~(PTE_WRITABLE | PTE_NX);
    if (writable && !(*pte & PTE_COW)) *pte |= PTE_WRITABLE;
    if (!executable) *pte |= PTE_NX;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}


/* **写像を外すだけ。物理ページは返さない (2026-09-10)。**
 *
 * 契約は kernel/sys_mmap.c の mmap_drop_page に明記してある —— 呼び手が
 * arch_vm_get_phys してから自分で pmm_free する。aarch64 も riscv64 も
 * これに従う (riscv64 は 2026-09-10 まで外れていた)。
 *
 * **ここは「vmm.c に unmap が無い」としてスタブだったが、中身は在った** ——
 * kernel/x86_64/sys_vm.c の unmap_one_page がそれで、あちらは自分で
 * pmm_free もする。**契約に合わせて解放しない形で書く。**
 *
 * 2MB ページは mmap が作らないので触らない (unmap_one_page と同じ)。 */
static inline void arch_vm_unmap_page(arch_address_space_t address_space, uint64_t vaddr) {
    uint64_t* pml4 = arch_vm_address_space_root(address_space);
    uint64_t* pdp;
    uint64_t* pd;
    uint64_t* pt;
    uint64_t* pte;

    if (!pml4) return;
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return;
    pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return;
    pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return;
    pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    pte = &pt[PT_IDX(vaddr)];
    if (!(*pte & PTE_PRESENT)) return;
    *pte = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* **属性だけ差し替える。物理アドレスはそのまま (2026-09-12)。**
 *
 * ここは「vmm.c に直の API が無い」としてスタブだった。**呼び手は在る** ——
 * kernel/elf.c は 3 アーキとも組んでいるので、段が同じページを跨いだとき、
 * **x86 だけ何もせず先に触った段の権限が残り**、aarch64 / riscv64 は後の段で
 * 上書きされていた。**3 アーキが 3 通りに振る舞っていた。**
 *
 * 呼び手は権限の和を渡してくる (kernel/elf.c の elf_page_vmm_flags) ので、
 * ここは名前のとおり置き換えるだけでよい。aarch64 / riscv64 の実装と同じ。
 *
 * **ソフトウェアのビット (PTE_COW) は残らない。**aarch64 の実装も同じで、
 * 呼び手は exec が組み立て中の新しいアドレス空間しか触らないので COW は
 * 立っていない。2MB ページは触らない (arch_vm_unmap_page と同じ)。
 *
 * ★ **葉だけ書いても効かない (2026-09-12 に実機で踏んだ)。**x86 の実効権限は
 * PML4E / PDPE / PDE / PTE の **AND** なので、先に貼った段の権限で作られた
 * 中間段が W を持っていなければ、葉に PTE_WRITABLE を立てても読み取り専用の
 * ままになる。最初に書いた実装は葉だけを書いており、
 *
 *     DBG pte before=0000000022109005   ← P|U
 *     DBG pte after =0000000022109007   ← P|U|W  (葉は直っている)
 *     #PF(User): write-to-nonwritable at 0x00000000004009B0
 *
 * と、**PTE は直っているのに書けなかった** (tests/x86_straddle_smoke.sh)。
 * vmm_map_page は get_next_level が既存の中間段にも PTE_USER / PTE_WRITABLE を
 * 足すので、**葉の物理アドレスを取り直して vmm_map_page に渡す。**
 * aarch64 / riscv64 に同じ話は無い —— riscv64 の非葉 PTE は R=W=X=0 で
 * 権限を持たず、aarch64 の表記述子も既定 (APTable=0) が最も緩い */
static inline void arch_vm_update_page_flags(arch_address_space_t address_space, uint64_t vaddr, uint64_t new_flags) {
    uint64_t* pml4 = arch_vm_address_space_root(address_space);
    uint64_t* pdp;
    uint64_t* pd;
    uint64_t* pt;
    uint64_t* pte;

    if (!pml4) return;
    if (!(pml4[PML4_IDX(vaddr)] & PTE_PRESENT)) return;
    pdp = (uint64_t*)PHYS_TO_VIRT(pml4[PML4_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pdp[PDP_IDX(vaddr)] & PTE_PRESENT)) return;
    pd = (uint64_t*)PHYS_TO_VIRT(pdp[PDP_IDX(vaddr)] & PTE_ADDR_MASK);
    if (!(pd[PD_IDX(vaddr)] & PTE_PRESENT)) return;
    if (pd[PD_IDX(vaddr)] & PTE_HUGE) return;
    pt = (uint64_t*)PHYS_TO_VIRT(pd[PD_IDX(vaddr)] & PTE_ADDR_MASK);
    pte = &pt[PT_IDX(vaddr)];
    if (!(*pte & PTE_PRESENT)) return;
    /* 中間段を広げるのは vmm_map_page に任せる。invlpg もあちらが撃つ */
    vmm_map_page(pml4, vaddr, *pte & PTE_ADDR_MASK, new_flags);
}

#endif
