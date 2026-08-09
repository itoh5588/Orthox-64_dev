#ifndef ORTHOX_ARCH_AARCH64_DTB_H
#define ORTHOX_ARCH_AARCH64_DTB_H

#include <stdint.h>

/* Devicetree Blob (FDT) の読み取り。
 *
 * 中身はすべてビッグエンディアンで入っている。AArch64 はリトルエンディアンで
 * 動かすので、32bit / 64bit のどちらもバイトを並べ替えて読む必要がある。
 *
 * 構造:
 *   ヘッダ            マジックと各ブロックの位置
 *   struct ブロック   トークンの並び。ノードとプロパティの木
 *   strings ブロック  プロパティ名の文字列。struct 側はここへの offset を持つ
 */

#define AARCH64_FDT_MAGIC 0xd00dfeedU

#define AARCH64_FDT_BEGIN_NODE  0x00000001U
#define AARCH64_FDT_END_NODE    0x00000002U
#define AARCH64_FDT_PROP        0x00000003U
#define AARCH64_FDT_NOP         0x00000004U
#define AARCH64_FDT_END         0x00000009U

typedef struct aarch64_fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} aarch64_fdt_header_t;

uint32_t aarch64_dtb_read_be32(const void* addr);
uint64_t aarch64_dtb_read_be64(const void* addr);
int aarch64_dtb_valid(uint64_t dtb_pa);
uint32_t aarch64_dtb_total_size(uint64_t dtb_pa);

/* x0 で渡された値を優先し、駄目ならマジックを目印に探す。
 * 見つからなければ 0 を返す */
uint64_t aarch64_dtb_find(uint64_t hint);

/* DTB を走査して boot info を埋める。dtb_pa が妥当でなければ何もしない */
void aarch64_dtb_scan(uint64_t dtb_pa);

#endif
