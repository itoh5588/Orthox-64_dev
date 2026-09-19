#ifndef PMM_CORE_H
#define PMM_CORE_H
/*
 * 物理ページの配り方 (3 アーキ共通、2026-09-19)。
 *
 * **判断は共通、置き場はアーキ。**vm_cow と同じ分け方にした。
 * 以前は 3 アーキが別々のビットマップ確保を持っていて、aarch64 だけが
 * 2026-08-30 の実機プロファイルで直した next-fit を持っていた。x86 と
 * riscv64 は毎回ページ 0 から 1 ビットずつ探し、x86 (QEMU -m 2G) では
 * 1 回の確保で平均 141,660 ページを舐めていた (日報2026-09-19)。
 *
 * 共通層 (kernel/pmm_core.c) が持つもの:
 *   - ビットマップと参照カウントの読み書き、1 本のロック
 *   - 探し方 (next-fit、解放で起点を戻す、大きな連なりは先頭から)
 *   - 走査の長さの計器
 *   - 共有層の pmm_alloc / pmm_free / pmm_incref / pmm_get_ref と
 *     使用量の問い合わせ (include/pmm.h)
 *
 * アーキが持つもの:
 *   - 管理する範囲と、管理情報 (ビットマップ + 参照カウント) の置き場を
 *     決めて pmm_core_setup に渡す。空きにしてよい範囲を
 *     pmm_core_mark_free で、予約を pmm_core_mark_used で塗る
 *   - arch_pmm_meta_ptr: 管理情報の物理アドレスを、いま触れるポインタに
 *     する。共通層は pmm_core_setup で引いて覚えておく。aarch64 は MMU を
 *     入れる前後で変わるので、上位 VA へ移ったところで pmm_core_rebind を呼ぶ
 *   - arch_pmm_after_alloc: pmm_alloc で取ったページへの後始末。aarch64 は
 *     0 で埋める。x86 / riscv64 は何もしない (呼ぶ側が埋めている)
 *
 * **番地はすべて物理。**管理範囲の外 (フレームバッファ、MMIO、予約) の
 * ページは参照カウント 0 として扱い、incref / free は無視する。vm_cow は
 * これを「pmm が持たないページ」と見て共有のまま渡す。
 */
#include <stdint.h>

/* 管理する範囲 [base_pa, base_pa + pages * 4KiB) と管理情報の置き場。
 * bitmap_pa には (pages + 7) / 8 バイト、refcount_pa には pages 個の
 * uint16_t が要る。**全ページを使用中、参照カウント 0 で始める。**
 * 管理情報が範囲の中にあるなら、そのページは呼び手が mark_used する */
void pmm_core_setup(uint64_t base_pa, uint64_t pages,
                    uint64_t bitmap_pa, uint64_t refcount_pa);

/* 管理情報のポインタを arch_pmm_meta_ptr で引き直す (aarch64 が上位 VA へ
 * 移ったとき。恒等マッピングを外す前に呼ぶこと) */
void pmm_core_rebind(void);

/* [pa, pa + bytes) のうち範囲に入るページを空き / 使用中にする。
 * 端は内側に丸める (free) / 外側に広げる (used) */
void pmm_core_mark_free(uint64_t pa, uint64_t bytes);
void pmm_core_mark_used(uint64_t pa, uint64_t bytes);

/* 1 ページを使用中にし、参照カウントを 1 にする。空きでなければ 0 を返す
 * (x86 のページ 0 や ISA DMA 用ページの予約) */
int pmm_core_reserve_page(uint64_t pa);

/* 参照カウントに触らずに pages 枚の連なりを取る / 返す。0 埋めもしない。
 * aarch64 のページテーブルや DMA のように、共有層の外で持つもの用 */
uint64_t pmm_core_claim(uint64_t pages);
void pmm_core_release(uint64_t pa, uint64_t pages);

uint64_t pmm_core_base(void);
uint64_t pmm_core_total(void);
uint64_t pmm_core_used(void);

/* 計器。前回読んでからの確保回数、走査したページ数、2 周目まで行った回数、
 * いまの起点。読むと 0 に戻る (起点を除く) */
struct pmm_core_scan_stat {
    uint64_t calls;
    uint64_t scanned;
    uint64_t wraps;
    uint64_t next_page;
};
void pmm_core_take_scan_stat(struct pmm_core_scan_stat* out);

/* ---- アーキが用意するもの ---- */
void* arch_pmm_meta_ptr(uint64_t pa);
void arch_pmm_after_alloc(uint64_t pa, uint64_t pages);

#endif
