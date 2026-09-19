#ifndef ORTHOX_VM_COW_H
#define ORTHOX_VM_COW_H

#include <stdint.h>
#include "arch_vm.h"

/* ---- fork の copy-on-write (3 アーキ共通, 2026-09-19) ----------------------
 *
 * **判断と写しはここ (kernel/vm_cow.c)、アーキは部品だけ出す。**Linux の
 * 分担 (mm/memory.c の copy_present_pte / do_wp_page が共通で、アーキは
 * ptep_set_wrprotect などの PTE 操作と TLB の破棄だけ) に合わせた。
 * 以前は x86 の kernel/x86_64/vmm.c に閉じていたため、aarch64 / riscv64 を
 * 作るときに引き継がれなかった (日報2026-09-19)。
 *
 * **Linux との違い: VMA が無い。**Linux は「書き込み可で共有でない領域」
 * (is_cow_mapping) で CoW かどうかを決めるが、Orthox には領域の記録が無く
 * PTE しか無い。そこで
 *
 *   - CoW の印 = 「本来は書けるが、共有中なので写すまで書かせない」
 *     (アーキのソフトウェア用ビット)
 *   - 共有中 = pmm の参照カウントが 2 以上
 *
 * とし、mprotect で書き込み可にするときも参照カウントを見て印に振り替える
 * (vm_cow_protect_page)。fork の時点で読み取り専用だったページも、後から
 * 書き込み可にした瞬間に写す対象になる。
 *
 * **pmm が持っていないページは CoW にしない。**フレームバッファのような
 * 装置のメモリは参照カウントが 0 で、写すと画面に届かなくなる。共有の
 * まま子へ渡す (Linux の VM_PFNMAP と同じ扱い)。
 *
 * アーキが出す部品 (include/<arch>/vm.h):
 *
 *   uint64_t* arch_vm_user_leaf(as, va, &pages)
 *       ユーザーの葉 PTE (有効なもの) を指すポインタ。無ければ 0。
 *       pages は葉が覆うページ数 (4KB なら 1、x86 の 2MB なら 512)
 *   uint64_t arch_pte_phys(e) / int arch_pte_writable(e) / int arch_pte_cow(e)
 *   uint64_t arch_pte_mkcow(e)             書き込み禁止にして印を立てる
 *   uint64_t arch_pte_clear_cow(e)         印だけ落とす
 *   uint64_t arch_pte_mkwrite(e, phys)     物理を差し替え、印を落として書き込み可
 *   void arch_vm_flush_user_page(as, va)   **全 CPU から** va の変換を捨てる
 */

/* vm_cow_write_fault の戻り値 */
#define VM_COW_HANDLED   0    /* 処理した。落ちた命令をやり直させる */
#define VM_COW_NOT_COW  (-1)  /* CoW ではない本物の違反 */
#define VM_COW_NOMEM    (-2)  /* 写す先のページが取れない */

/* fork: 親の葉 *parent_leaf を子へ渡す形にして返す (親の葉も書き換える)。
 * pages は葉が覆うページ数。呼び手は親のアドレス空間全体を写し終えたら
 * TLB を捨てること (親が書き込み可のまま古い変換で書けてしまう) */
uint64_t vm_cow_share_leaf(uint64_t* parent_leaf, uint64_t pages);

/* 書き込みで落ちたときに呼ぶ。ユーザーからでもカーネルからでもよい
 * (カーネルは read(2) などでユーザーのバッファへ直接書くため) */
int vm_cow_write_fault(arch_address_space_t as, uint64_t vaddr);

/* mprotect 用。共有中のページは書き込み可にせず印に振り替える */
void vm_cow_protect_page(arch_address_space_t as, uint64_t vaddr,
                         int writable, int executable);

/* mremap 用。印の立ったページは「書けた」と読む */
int vm_cow_get_page_prot(arch_address_space_t as, uint64_t vaddr,
                         int* writable, int* executable);

#endif
