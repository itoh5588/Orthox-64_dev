#include <stdint.h>
#include <stddef.h>
#include "vm_cow.h"
#include "pmm.h"
#include "vmm.h"      /* PHYS_TO_VIRT。3 アーキとも g_hhdm_offset を持つ */
#include "string.h"

/* 考え方と部品の一覧は include/vm_cow.h */

/* **共有中か。**pmm が持っていないページ (参照カウント 0) は共有扱いにしない */
static int vm_cow_page_shared(uint64_t phys) {
    return pmm_get_ref((void*)(uintptr_t)phys) > 1;
}

/* Linux の copy_present_pte に当たる。
 *
 * **読み取り専用のページにも参照を足す。**text は印を立てずに共有する
 * だけで、書き込みで落ちれば本物の違反。後から mprotect で書き込み可に
 * されたら vm_cow_protect_page が参照カウントを見て印に振り替える */
uint64_t vm_cow_share_leaf(uint64_t* parent_leaf, uint64_t pages) {
    uint64_t e = *parent_leaf;
    uint64_t phys = arch_pte_phys(e);

    /* 装置のメモリ (フレームバッファ等) は共有のまま渡す。写すと画面に
     * 届かなくなるうえ、フォルト処理が参照カウント 0 のページを扱う */
    if (pmm_get_ref((void*)(uintptr_t)phys) == 0) return e;

    if (arch_pte_writable(e)) {
        e = arch_pte_mkcow(e);
        *parent_leaf = e;
    }
    for (uint64_t i = 0; i < pages; i++) {
        pmm_incref((void*)(uintptr_t)(phys + i * PAGE_SIZE));
    }
    return e;
}

/* Linux の do_wp_page に当たる。
 *
 * **順番を崩さない: 新しい葉を書く → TLB を捨てる → 古いページを手放す。**
 * 先に手放すと、相手が参照 1 を見て書き込み可にした瞬間、こちらの CPU に
 * 残った古い変換から相手の書いた中身が見える。
 *
 * **参照を読んでから書くまでに相手が動いてもよい。**2 つのプロセスが同時に
 * 落ちると両方が 2 を見て両方が写し、元のページは 2 回の pmm_free で返る。
 * 片方が 1 を見るのは、相手が自分の葉を差し替えて手放した後に限られる */
int vm_cow_write_fault(arch_address_space_t as, uint64_t vaddr) {
    uint64_t pages = 0;
    uint64_t* leaf = arch_vm_user_leaf(as, vaddr, &pages);
    uint64_t e;
    uint64_t base;
    uint64_t old_phys;

    if (!leaf) return VM_COW_NOT_COW;
    e = *leaf;
    base = vaddr & ~(pages * PAGE_SIZE - 1ULL);

    if (!arch_pte_cow(e)) {
        /* **もう書ける = 別の CPU に古い読み取り専用の変換が残っていた。**
         * 捨てて、やり直させれば通る (Linux の spurious fault と同じ扱い) */
        if (arch_pte_writable(e)) {
            arch_vm_flush_user_page(as, base);
            return VM_COW_HANDLED;
        }
        return VM_COW_NOT_COW;
    }

    old_phys = arch_pte_phys(e);
    if (vm_cow_page_shared(old_phys)) {
        void* new_phys = pmm_alloc(pages);
        if (!new_phys) return VM_COW_NOMEM;
        memcpy(PHYS_TO_VIRT(new_phys), PHYS_TO_VIRT(old_phys), pages * PAGE_SIZE);
        /* **写した先を I-cache と揃える。**以前は aarch64 / riscv64 の fork が
         * 全ページを写すところ (aarch64_vm_copy_page) でやっていた。CoW では
         * 写すのがここだけになるので、ここに無いと text を持つページが
         * 古い命令のまま走りうる (日報2026-08-23 の V-2)。x86 では空 */
        arch_sync_icache_range(PHYS_TO_VIRT(new_phys), pages * PAGE_SIZE);
        *leaf = arch_pte_mkwrite(e, (uint64_t)(uintptr_t)new_phys);
        arch_vm_flush_user_page(as, base);
        pmm_free((void*)(uintptr_t)old_phys, pages);
    } else {
        /* 最後の 1 人。写さずに書き込み可へ戻す */
        *leaf = arch_pte_mkwrite(e, old_phys);
        arch_vm_flush_user_page(as, base);
    }
    return VM_COW_HANDLED;
}

/* **書き込み可にするとき、共有中なら印に振り替える。**
 * 読み取り専用にするときは印も落とす —— 後で書き込み可に戻されたら、
 * その時点の参照カウントで決め直せばよい。
 *
 * TLB は呼び手 (sys_mprotect) がまとめて捨てる */
void vm_cow_protect_page(arch_address_space_t as, uint64_t vaddr,
                         int writable, int executable) {
    uint64_t pages = 0;
    uint64_t* leaf = arch_vm_user_leaf(as, vaddr, &pages);
    int cow = 0;

    if (!leaf) return;
    if (writable) cow = arch_pte_cow(*leaf) || vm_cow_page_shared(arch_pte_phys(*leaf));
    arch_vm_protect_page(as, vaddr, writable && !cow, executable);

    /* arch_vm_protect_page が葉を貼り直すアーキもあるので引き直す */
    leaf = arch_vm_user_leaf(as, vaddr, &pages);
    if (!leaf) return;
    *leaf = cow ? arch_pte_mkcow(*leaf) : arch_pte_clear_cow(*leaf);
}

int vm_cow_get_page_prot(arch_address_space_t as, uint64_t vaddr,
                         int* writable, int* executable) {
    uint64_t pages = 0;
    uint64_t* leaf;
    int rc = arch_vm_get_page_prot(as, vaddr, writable, executable);

    if (rc < 0 || !writable) return rc;
    leaf = arch_vm_user_leaf(as, vaddr, &pages);
    if (leaf && arch_pte_cow(*leaf)) *writable = 1;
    return rc;
}
