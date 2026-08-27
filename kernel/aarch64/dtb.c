/*
 * Devicetree (FDT) パーサ。
 *
 * **アドレスを直書きしたままだと Raspberry Pi 4 で必ず破綻する。** RAM も
 * PL011 も GIC も、QEMU virt とは全部違う場所にある。ここで DTB から取る。
 *
 * 作りは kernel/riscv64/boot.c の riscv64_dtb_scan を土台にしているが、
 * **2 点だけ変えてある**。どちらも実測で痛い目を見た所:
 *
 *   1. #address-cells / #size-cells を実際に読む。
 *      riscv64 版は reg の長さから「2 セル / 2 セル だろう」と推測している。
 *      QEMU virt では当たるが、Pi 4 の DT はノードによって違うので破綻する。
 *
 *   2. reg の 2 組目以降も読む。
 *      日報2026-08-09 §1 で、GIC の reg の 1 組目 (Distributor) しか読まず
 *      **CPU Interface (reg[1]) を取りこぼした**。GIC は 2 組そろって初めて
 *      使える。「読んだ」で満足せず、必要な項目が全部取れたかまで見る。
 *
 * DTB の中身はすべてビッグエンディアン。AArch64 はリトルエンディアンで
 * 動かすので、読むたびに並べ替える。
 */
#include <stdint.h>
#include "aarch64/boot.h"
#include "aarch64/dtb.h"

uint32_t aarch64_dtb_read_be32(const void* addr) {
    const uint8_t* p = (const uint8_t*)addr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

uint64_t aarch64_dtb_read_be64(const void* addr) {
    const uint8_t* p = (const uint8_t*)addr;
    return ((uint64_t)aarch64_dtb_read_be32(p) << 32) |
           (uint64_t)aarch64_dtb_read_be32(p + 4);
}

int aarch64_dtb_valid(uint64_t dtb_pa) {
    if (!dtb_pa) return 0;
    return aarch64_dtb_read_be32((const void*)(uintptr_t)dtb_pa) == AARCH64_FDT_MAGIC;
}

uint32_t aarch64_dtb_total_size(uint64_t dtb_pa) {
    const aarch64_fdt_header_t* h = (const aarch64_fdt_header_t*)(uintptr_t)dtb_pa;
    if (!aarch64_dtb_valid(dtb_pa)) return 0;
    return aarch64_dtb_read_be32(&h->totalsize);
}

/* QEMU に ELF を -kernel で渡すと x0 に DTB のアドレスが来ない (実測。
 * -dtb を足しても変わらない)。Linux の boot protocol は Image 形式向けで、
 * ELF は「素のバイナリ」として扱われるため。
 *
 * **実機の Pi 4 のファームウェアは x0 で渡す。** つまりマジックの走査は
 * QEMU 用の回避策で、実機では素直に x0 を使える。x0 を先に見るのはそのため。 */
uint64_t aarch64_dtb_find(uint64_t hint) {
    static const uint64_t candidates[] = {
        0x40000000ULL,   /* RAM 先頭。QEMU virt が Linux 起動で置く場所 */
        0x48000000ULL,   /* RAM 先頭 + 128MB */
        0x44000000ULL,
    };
    if (aarch64_dtb_valid(hint)) return hint;
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (aarch64_dtb_valid(candidates[i])) return candidates[i];
    }
    return 0;
}

static uint32_t align_up4(uint32_t v) { return (v + 3U) & ~3U; }

static int str_eq(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* compatible は "a\0b\0c\0" と NUL 区切りで複数入っている。
 * どれか 1 つでも一致すればよい */
static int compatible_has(const char* data, uint32_t len, const char* needle) {
    uint32_t i = 0;
    if (!data || !needle) return 0;
    while (i < len) {
        const char* entry = data + i;
        uint32_t entry_len = 0;
        while (i + entry_len < len && entry[entry_len] != '\0') entry_len++;
        if (i + entry_len >= len) break;
        if (str_eq(entry, needle)) return 1;
        i += entry_len + 1U;
    }
    return 0;
}

/* reg は「アドレス cells 個 + サイズ cells 個」を 1 組として繰り返す。
 * **組の数はノードによって違う** (GIC は 2 組)。index 番目の組を取り出す。
 * 戻り値: 取れたら 1 */
static int reg_entry(const uint8_t* data, uint32_t len,
                     uint32_t addr_cells, uint32_t size_cells,
                     uint32_t index, uint64_t* out_base, uint64_t* out_size) {
    uint32_t stride_cells = addr_cells + size_cells;
    uint32_t stride_bytes = stride_cells * 4U;
    uint32_t off;

    if (!data || stride_bytes == 0) return 0;
    if (addr_cells == 0 || addr_cells > 2 || size_cells > 2) return 0;

    off = index * stride_bytes;
    if (off + stride_bytes > len) return 0;

    *out_base = (addr_cells == 2) ? aarch64_dtb_read_be64(data + off)
                                  : (uint64_t)aarch64_dtb_read_be32(data + off);
    off += addr_cells * 4U;

    if (size_cells == 0) {
        *out_size = 0;
    } else {
        *out_size = (size_cells == 2) ? aarch64_dtb_read_be64(data + off)
                                      : (uint64_t)aarch64_dtb_read_be32(data + off);
    }
    return 1;
}

/* ノード名が "name@address" の "name" 部分と一致するか */
static int node_name_is(const char* node, const char* name) {
    while (*name) {
        if (*node != *name) return 0;
        node++; name++;
    }
    return *node == '\0' || *node == '@';
}

/* 木の深さごとに #address-cells / #size-cells を覚えておく。
 * 子ノードの reg は**親の**セル数で解釈するので、親の値が要る */
#define MAX_DEPTH 16

/* いま見ているノードについて溜めておくもの。END_NODE で確定させる。
 *
 * **深さごとに持つこと。** 1 組しか持たずに書いていて実際に落ちた:
 * QEMU virt の intc@8000000 には子ノード v2m@8020000 があり、子の
 * BEGIN_NODE で親の compatible と reg が消え、親の END_NODE に着いたときに
 * 何も残っていなかった。GIC だけ「既定値」のまま進んでいた
 * (値が既定値と同じだったので出力は正しく見えた。フラグを持たせて
 * いなければ気づけない)。
 *
 * kernel/riscv64/boot.c の riscv64_dtb_scan は 1 組しか持っていないが、
 * あちらが拾う uart / virtio / memory は子ノードを持たないので露見しない。 */
/* ---- ranges (バスアドレス -> 親のアドレス) の変換 ------------------------
 *
 * **QEMU virt では要らないが、Raspberry Pi では必須。**
 *
 * QEMU virt は周辺をルート直下に置くので、reg にそのまま物理が入っている。
 * Pi は /soc というバスノードの下に置き、reg には**バスアドレス**を書く:
 *
 *   soc {
 *       #address-cells = <1>; #size-cells = <1>;
 *       ranges = <0x7e000000  0x0 0xfe000000  0x1800000>;
 *                 ^子のアドレス ^親のアドレス  ^長さ
 *       serial@7e201000 { reg = <0x7e201000 0x200>; };   -> 物理 0xfe201000
 *   };
 *
 * **変換しないと 0x7e201000 を叩きに行って沈黙する。** どこにも繋がって
 * いないアドレスなので、エラーも出ない。
 *
 * ranges が空 (長さ 0) なら「そのまま通す」の意味。プロパティが無い場合は
 * 本来「変換できない」だが、ここでは素通しにする — QEMU virt のように
 * バスノードを持たない木で余計な失敗をしないため。 */
typedef struct dtb_range {
    uint64_t child;
    uint64_t parent;
    uint64_t size;
} dtb_range_t;

#define MAX_RANGES_PER_NODE 4

typedef struct dtb_node_state {
    int is_uart;
    int is_gic;
    int is_virtio;
    /* Raspberry Pi 4 の SD カードコントローラ (EMMC2)。**Pi の SD は
     * 世代で別物**で、Pi 4 の実物 DTB では旧 mmc@7e300000 が disabled、
     * 有効なのは /emmc2bus/mmc@7e340000 のほう。
     * emmc2bus は soc とは**別のバスノード**で子のセル数も違う
     * (emmc2bus は子 2 / soc は子 1) ので、ranges 変換をここでも通す */
    int is_emmc2;
    /* VideoCore の mailbox。**画面を取るのに要る唯一の口。**
     * /soc/mailbox@7e00b880 で、soc の ranges を通して 0xfe00b880 になる */
    int is_mbox;
    /* PCIe のホストブリッジ (ECAM)。**QEMU virt にはあるが raspi4b には無い。**
     * reg が ECAM の窓、ranges が BAR を置ける空間を指す */
    int is_pcie;
    /* Raspberry Pi 4 の PCIe。**ECAM とは別扱い** — 設定空間の出し方が違う */
    int is_pcie_brcm;
    int is_memory;
    int is_timer;
    int is_cpu;
    /* **status = "disabled" のノードを採用しないための印。**
     * Pi 4 の DTB には arm,pl011 を名乗るノードが 5 つあり、有効なのは
     * serial@7e201000 だけ。残り 4 つは disabled。見ないと最後に見つけた
     * 無効なポート (0xfe201a00) を掴んで**実機で沈黙する**。
     * **プロパティが無ければ有効** (DT の既定) */
    int is_disabled;
    const uint8_t* reg_data;
    uint32_t reg_len;
    const uint8_t* intr_data;
    uint32_t intr_len;
    /* cpu@N の起こし方 (SMP)。**`/cpus` の親にも enable-method が在る**
     * (Pi 4 は "brcm,bcm2836-smp") が、使うのは cpu@N 側のほう */
    const char* enable_method;
    uint32_t enable_method_len;
    const uint8_t* release_addr_data;
    uint32_t release_addr_len;
    /* **生の ranges。** PCI の ranges は子が 3 セルで、下の汎用の解釈
     * (子 <= 2 セル) では読めない。PCIe のときだけ自分で読み直す */
    const uint8_t* ranges_data;
    uint32_t ranges_len;
    /* **内向きの窓。**PCI の dma-ranges も子が 3 セルなので自分で読む */
    const uint8_t* dma_ranges_data;
    uint32_t dma_ranges_len;
    /* このノードが**バスとして**持つ ranges。子の reg に適用する */
    dtb_range_t ranges[MAX_RANGES_PER_NODE];
    uint32_t range_count;
    int has_ranges;      /* プロパティが在ったか (空の ranges と区別する) */
} dtb_node_state_t;

static void node_state_clear(dtb_node_state_t* n) {
    n->is_uart = n->is_gic = n->is_virtio = 0;
    n->is_emmc2 = 0;
    n->is_mbox = 0;
    n->is_pcie = 0;
    n->is_pcie_brcm = 0;
    n->is_memory = n->is_timer = n->is_cpu = 0;
    n->is_disabled = 0;
    n->reg_data = 0;
    n->reg_len = 0;
    n->intr_data = 0;
    n->intr_len = 0;
    n->enable_method = 0;
    n->enable_method_len = 0;
    n->release_addr_data = 0;
    n->release_addr_len = 0;
    n->range_count = 0;
    n->has_ranges = 0;
    n->ranges_data = 0;
    n->ranges_len = 0;
    n->dma_ranges_data = 0;
    n->dma_ranges_len = 0;
}

/* 深さ depth のノードの reg を、ルートから見た物理アドレスに直す。
 * **親から順に適用する** (子のバス -> 親のバス -> ... -> 物理)。
 * 変換に当たらなければそのまま返す (素通しの木で壊さないため)。 */
static uint64_t dtb_translate(const dtb_node_state_t* nodes, int depth, uint64_t addr) {
    for (int d = depth - 1; d >= 0; d--) {
        const dtb_node_state_t* bus = &nodes[d];
        if (!bus->has_ranges || bus->range_count == 0) continue;
        for (uint32_t i = 0; i < bus->range_count; i++) {
            const dtb_range_t* r = &bus->ranges[i];
            if (addr >= r->child && addr - r->child < r->size) {
                addr = r->parent + (addr - r->child);
                break;
            }
        }
    }
    return addr;
}

/* reg を読んで **その場で物理へ直す**。
 *
 * **reg_entry を直接呼ばないこと。** 呼び出しが 6 箇所あり、片方だけ変換を
 * 忘れると「UART は当たるが GIC は当たらない」のような形で静かに壊れる。
 * ここを通す限り、増やしても直し忘れが起きない。 */
static int reg_entry_phys(const dtb_node_state_t* nodes, int depth,
                          const uint8_t* data, uint32_t len,
                          uint32_t addr_cells, uint32_t size_cells,
                          uint32_t index, uint64_t* out_base, uint64_t* out_size) {
    if (!reg_entry(data, len, addr_cells, size_cells, index, out_base, out_size)) return 0;
    *out_base = dtb_translate(nodes, depth, *out_base);
    return 1;
}

void aarch64_dtb_scan(uint64_t dtb_pa) {
    const aarch64_fdt_header_t* header = (const aarch64_fdt_header_t*)(uintptr_t)dtb_pa;
    aarch64_boot_info_t* info = aarch64_boot_info_mut();
    uint32_t struct_off, struct_size, strings_off;
    const uint8_t* struct_base;
    const char* strings_base;
    uint32_t off = 0;
    int depth = 0;

    /* 既定は 2/2。DT の仕様上の既定は 2/1 だが、ルートで必ず明示されるので
     * ここに落ちてくることは通常無い */
    uint32_t addr_cells[MAX_DEPTH];
    uint32_t size_cells[MAX_DEPTH];

    dtb_node_state_t nodes[MAX_DEPTH];

    /* virtio-mmio は本数が多く並ぶので、まとめてから確定させる */
    uint64_t virtio_min = 0, virtio_max = 0;
    uint32_t virtio_count = 0;
    /* **スロットごとの割り込み番号。** 最小アドレスと最大アドレスの
     * ノードのぶんだけ覚えておき、後で「差 == 本数-1」を確かめる。
     * 成り立てば「スロット i の INTID = base + i」と言える */
    uint32_t virtio_irq_at_min = 0, virtio_irq_at_max = 0;
    int virtio_irq_seen = 0;

    if (!aarch64_dtb_valid(dtb_pa)) return;

    for (int i = 0; i < MAX_DEPTH; i++) {
        addr_cells[i] = 2;
        size_cells[i] = 2;
        node_state_clear(&nodes[i]);
    }

    struct_off   = aarch64_dtb_read_be32(&header->off_dt_struct);
    struct_size  = aarch64_dtb_read_be32(&header->size_dt_struct);
    strings_off  = aarch64_dtb_read_be32(&header->off_dt_strings);
    struct_base  = (const uint8_t*)(uintptr_t)(dtb_pa + struct_off);
    strings_base = (const char*)(uintptr_t)(dtb_pa + strings_off);

    while (off + 4U <= struct_size) {
        uint32_t token = aarch64_dtb_read_be32(struct_base + off);
        off += 4U;

        if (token == AARCH64_FDT_BEGIN_NODE) {
            const char* node_name = (const char*)(struct_base + off);
            while (off < struct_size && struct_base[off] != 0) off++;
            off++;
            off = align_up4(off);

            if (depth + 1 < MAX_DEPTH) {
                /* 明示が無ければ親を引き継ぐ。DT の規則どおり */
                addr_cells[depth + 1] = addr_cells[depth];
                size_cells[depth + 1] = size_cells[depth];
            }
            depth++;

            /* **深さごとの枠を使う。** 子ノードに入っても親の状態が消えない */
            if (depth < MAX_DEPTH) {
                node_state_clear(&nodes[depth]);
                nodes[depth].is_cpu = node_name_is(node_name, "cpu");
            }
            continue;
        }

        if (token == AARCH64_FDT_END_NODE) {
            /* **reg の解釈は親のセル数で行う。** 自分のセル数ではない */
            int parent = (depth >= 1 && depth - 1 < MAX_DEPTH) ? depth - 1 : 0;
            uint32_t ac = addr_cells[parent];
            uint32_t sc = size_cells[parent];
            uint64_t base = 0, size = 0;
            const dtb_node_state_t* n;

            if (depth <= 0 || depth >= MAX_DEPTH) {
                if (depth > 0) depth--;
                continue;
            }
            n = &nodes[depth];

            /* **base == 0 を弾かないこと。** RAM が物理 0 から始まる機械では
             * それが正しい値で、Raspberry Pi 4 がまさにそれ (/memory@0)。
             * QEMU の virt は RAM が 0x40000000 からなので base が非 0 になり、
             * 以前の `base != 0` 条件でも通っていた。**実機でだけ既定値に
             * 退いていた。**
             *
             * 弾く根拠があるのは size == 0 だけ (配布 DTB の /memory@0 は
             * reg = <0 0 0> で、ファームウェアが起動時に書き換える。
             * 書き換えられていなければ size が 0 のまま)。
             *
             * **エントリは全部読む。** Pi 4 (4GB) は RAM が 2 つに割れていて、
             * 最初だけ読むと 942MB しか見えない */
            if (n->is_memory && !n->is_disabled && n->reg_data &&
                info->mem_range_count == 0) {
                uint64_t lo = 0, hi = 0;
                for (uint32_t idx = 0; idx < AARCH64_MAX_MEM_RANGES; idx++) {
                    if (!reg_entry_phys(nodes, depth, n->reg_data, n->reg_len,
                                        ac, sc, idx, &base, &size)) break;
                    if (size == 0) continue;
                    info->mem_range_base[info->mem_range_count] = base;
                    info->mem_range_size[info->mem_range_count] = size;
                    info->mem_range_count++;
                    if (hi == 0 || base < lo) lo = base;
                    if (base + size > hi) hi = base + size;
                }
                if (info->mem_range_count != 0) {
                    /* **穴を含む全体。**配ってよい範囲は mem_range_* のほう */
                    info->memory_base = lo;
                    info->memory_size = hi - lo;
                    info->flags |= AARCH64_BOOT_FLAG_MEMORY_FROM_DTB;
                }
            }

            if (n->is_uart && !n->is_disabled && n->reg_data &&
                reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &base, &size) && base != 0) {
                info->uart_base = base;
                info->flags |= AARCH64_BOOT_FLAG_UART_FROM_DTB;
                /* 受信割り込みの INTID (P3)。virtio と同じ形式で
                 * interrupts = <type num flags>、type 0 = SPI なので +32。
                 * **reg と別に立てる** — アドレスだけ DTB から取れて
                 * 割り込みは既定値、という半端な状態を見分けるため */
                if (n->intr_data && n->intr_len >= 12U &&
                    aarch64_dtb_read_be32(n->intr_data) == 0U) {
                    info->uart_intid = aarch64_dtb_read_be32(n->intr_data + 4) + 32U;
                    info->flags |= AARCH64_BOOT_FLAG_UART_IRQ_FROM_DTB;
                }
            }

            /* GIC は **2 組そろって初めて使える**。1 組目 = Distributor、
             * 2 組目 = CPU Interface。日報2026-08-09 §1 で落とした所 */
            if (n->is_gic && !n->is_disabled && n->reg_data) {
                uint64_t db = 0, ds = 0, cb = 0, cs = 0;
                if (reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &db, &ds) && db != 0) {
                    info->gicd_base = db;
                    if (ds) info->gicd_size = ds;
                    if (reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 1, &cb, &cs) && cb != 0) {
                        info->gicc_base = cb;
                        if (cs) info->gicc_size = cs;
                        info->flags |= AARCH64_BOOT_FLAG_GIC_FROM_DTB;
                    }
                    /* 2 組目が無ければ GIC_FROM_DTB は立てない。
                     * 「1 組しか取れなかった」を成功にしないため */
                }
            }

            /* **DTB に並ぶ順は昇順とは限らない** (QEMU virt は上のアドレスから
             * 並べる)。順に依存しないよう最小と最大だけ覚えておき、刻み幅は
             * ループの後で (max - min) / (本数 - 1) から出す */
            if (n->is_virtio && !n->is_disabled && n->reg_data &&
                reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &base, &size) && base != 0) {
                /* interrupts = <type num flags>。type 0 = SPI で、
                 * SPI の INTID は番号 + 32 (PPI が +16 なのと同じ理屈) */
                uint32_t intid = 0;
                int have_irq = 0;
                if (n->intr_data && n->intr_len >= 12U &&
                    aarch64_dtb_read_be32(n->intr_data) == 0U) {
                    intid = aarch64_dtb_read_be32(n->intr_data + 4) + 32U;
                    have_irq = 1;
                }
                if (virtio_count == 0 || base < virtio_min) {
                    virtio_min = base;
                    if (have_irq) { virtio_irq_at_min = intid; virtio_irq_seen |= 1; }
                }
                if (virtio_count == 0 || base > virtio_max) {
                    virtio_max = base;
                    if (have_irq) { virtio_irq_at_max = intid; virtio_irq_seen |= 2; }
                }
                virtio_count++;
            }

            /* EMMC2 (Pi 4 の SD カード)。**アドレスは /emmc2bus の ranges を
             * 通して初めて物理になる** (0x7e340000 -> 0xfe340000)。
             * 変換しないとどこにも繋がらない番地を叩いて沈黙する */
            if (n->is_emmc2 && !n->is_disabled && n->reg_data &&
                reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &base, &size) &&
                base != 0 && info->emmc2_base == 0) {
                info->emmc2_base = base;
                info->emmc2_size = size ? size : 0x100U;
                info->flags |= AARCH64_BOOT_FLAG_EMMC2_FROM_DTB;
                /* interrupts = <type num flags>。type 0 = SPI なので +32。
                 * **いまの実装はポーリングなので使っていない**が、
                 * 取れているかどうかを表示で見分けられるようにしておく */
                if (n->intr_data && n->intr_len >= 12U &&
                    aarch64_dtb_read_be32(n->intr_data) == 0U) {
                    info->emmc2_intid = aarch64_dtb_read_be32(n->intr_data + 4) + 32U;
                }

                /* **DMA で使う番地は reg とは別の変換を通る** (M-4c)。
                 *
                 * 実機の /emmc2bus は
                 *   dma-ranges = <0x0 0xc0000000  0x0 0x00000000  0x40000000>
                 * で、**コントローラから見た番地は物理 + 0xC0000000、
                 * 届く範囲は低位 1GB だけ。** ADMA2 の記述子にそのまま
                 * 物理を書くと、どこにも繋がらない場所を読み書きする。
                 *
                 * **直書きしない** — QEMU の raspi4b は旧 sdhci が /soc の
                 * 下に在って dma-ranges を持たず、そこでは差は 0 になる。
                 * 親 (バス) 側のプロパティなので nodes[parent] から読む */
                if (parent != depth && nodes[parent].dma_ranges_data) {
                    uint32_t cac = addr_cells[parent];
                    uint32_t pac = (parent > 0) ? addr_cells[parent - 1] : addr_cells[0];
                    uint32_t csc = size_cells[parent];
                    uint32_t need = (cac + pac + csc) * 4U;
                    if (cac <= 2 && pac <= 2 && csc <= 2 && cac != 0 &&
                        need != 0 && nodes[parent].dma_ranges_len >= need) {
                        const uint8_t* q = nodes[parent].dma_ranges_data;
                        uint64_t child, host, span;
                        child = (cac == 2) ? aarch64_dtb_read_be64(q)
                                           : aarch64_dtb_read_be32(q);
                        q += cac * 4U;
                        host = (pac == 2) ? aarch64_dtb_read_be64(q)
                                          : aarch64_dtb_read_be32(q);
                        q += pac * 4U;
                        span = (csc == 2) ? aarch64_dtb_read_be64(q)
                                          : ((csc == 1) ? aarch64_dtb_read_be32(q) : 0U);
                        info->emmc2_dma_offset = child - host;
                        info->emmc2_dma_limit = span ? (host + span) : 0;
                    }
                }
            }

            /* mailbox (VideoCore との窓口)。**EMMC2 と同じく ranges 変換が要る**
             * (0x7e00b880 -> 0xfe00b880)。割り込みは使わない — property の
             * やり取りは STATUS を見るポーリングで足りる */
            if (n->is_mbox && !n->is_disabled && n->reg_data &&
                reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &base, &size) &&
                base != 0 && info->mbox_base == 0) {
                (void)size;
                info->mbox_base = base;
                info->flags |= AARCH64_BOOT_FLAG_MBOX_FROM_DTB;
            }

            /* PCIe のホストブリッジ (ECAM)。
             *
             * **reg が設定空間、ranges の 32bit MMIO 窓が BAR の置き場。**
             * -kernel で直接起動すると BAR は未設定なので、こちらで配る
             * (kernel/aarch64/pci.c)。
             *
             * PCI の ranges は 1 組 7 セル:
             *   子 3 セル (先頭が空間の種別) + 親 2 セル + 長さ 2 セル
             * 種別の下位 2 ビットが 0b10 = 32bit MMIO。**そこだけ使う** —
             * I/O 空間は aarch64 に無く、64bit 窓は要らない */
            if (n->is_pcie && !n->is_disabled && n->reg_data &&
                reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &base, &size) &&
                base != 0 && info->pcie_ecam_base == 0) {
                info->pcie_ecam_base = base;
                info->pcie_ecam_size = size;
                info->flags |= AARCH64_BOOT_FLAG_PCIE_FROM_DTB;
                if (n->ranges_data && n->ranges_len >= 28U) {
                    for (uint32_t o = 0; o + 28U <= n->ranges_len; o += 28U) {
                        const uint8_t* r = n->ranges_data + o;
                        uint32_t space = aarch64_dtb_read_be32(r) & 0x03000000U;
                        if (space != 0x02000000U) continue;
                        info->pcie_mmio_base = aarch64_dtb_read_be64(r + 12);
                        info->pcie_mmio_size = aarch64_dtb_read_be64(r + 20);
                        break;
                    }
                }
            }

            /* Raspberry Pi 4 の PCIe。**番地は /scb の ranges を通して初めて
             * 物理になる** (EMMC2 と同じ理屈)。
             *
             * ranges は ECAM と同じ 7 セルの組。32bit MMIO の窓だけ拾う。
             * **PCI 側と CPU 側の番地が違う**ので両方覚える */
            if (n->is_pcie_brcm && !n->is_disabled && n->reg_data &&
                reg_entry_phys(nodes, depth, n->reg_data, n->reg_len, ac, sc, 0, &base, &size) &&
                base != 0 && info->pcie_brcm_base == 0) {
                info->pcie_brcm_base = base;
                info->pcie_brcm_size = size;
                info->flags |= AARCH64_BOOT_FLAG_PCIE_BRCM_FROM_DTB;
                if (n->ranges_data && n->ranges_len >= 28U) {
                    for (uint32_t o = 0; o + 28U <= n->ranges_len; o += 28U) {
                        const uint8_t* r = n->ranges_data + o;
                        if ((aarch64_dtb_read_be32(r) & 0x03000000U) != 0x02000000U) continue;
                        info->pcie_brcm_pci_base = aarch64_dtb_read_be64(r + 4);
                        info->pcie_brcm_cpu_base = aarch64_dtb_read_be64(r + 12);
                        info->pcie_brcm_win_size = aarch64_dtb_read_be64(r + 20);
                        break;
                    }
                }
                /* **内向きの窓。**組の形は ranges と同じ 7 セル */
                if (n->dma_ranges_data && n->dma_ranges_len >= 28U) {
                    for (uint32_t o = 0; o + 28U <= n->dma_ranges_len; o += 28U) {
                        const uint8_t* r = n->dma_ranges_data + o;
                        if ((aarch64_dtb_read_be32(r) & 0x03000000U) != 0x02000000U) continue;
                        info->pcie_dma_pci_base = aarch64_dtb_read_be64(r + 4);
                        info->pcie_dma_cpu_base = aarch64_dtb_read_be64(r + 12);
                        info->pcie_dma_size     = aarch64_dtb_read_be64(r + 20);
                        break;
                    }
                }
            }

            /* timer の interrupts は <type num flags> の 3 セル x 4 本
             * (secure / non-secure physical / virtual / hyp)。
             * 2 本目 (index 1) が非セキュア物理タイマ。
             * type 1 = PPI で、PPI の INTID は番号 + 16 */
            if (n->is_timer && n->intr_data && n->intr_len >= 24U) {
                uint32_t type = aarch64_dtb_read_be32(n->intr_data + 12);
                uint32_t num  = aarch64_dtb_read_be32(n->intr_data + 16);
                if (type == 1U) {
                    info->timer_intid = num + AARCH64_PPI_INTID_BASE;
                    info->flags |= AARCH64_BOOT_FLAG_TIMER_FROM_DTB;
                }
            }

            /* ---- cpu@N。**SMP の副コアの起こし方はここで決まる** --------
             *
             * 添字は DTB に並んでいた順。cpu_count は最後に「見つけた数」に
             * なるので、記録は増やす前の値を添字に使う。
             *
             * **reg は MPIDR の Aff0 で、アドレスではない。** だから
             * reg_entry_phys (ranges 変換つき) ではなく reg_entry を直に
             * 呼ぶ。**このファイルで reg_entry を直接呼んでよい唯一の場所。**
             * 変換を通すと /soc の ranges に当たって別の値になる。
             *
             * **cpu-release-addr は #address-cells に関係なく常に 8 バイト。**
             * Pi 4 は /cpus が #address-cells = 1 なのにこれは 64bit で
             * 入っている (2026-08-24 に tests/dtb の 2 本で実測)。
             * セル数で読むと上位 32bit を落とす */
            if (n->is_cpu) {
                uint32_t idx = info->cpu_count;
                if (idx < AARCH64_MAX_CPUS) {
                    uint64_t mpidr = 0, dummy = 0;
                    if (reg_entry(n->reg_data, n->reg_len, ac, sc, 0, &mpidr, &dummy)) {
                        info->cpu_mpidr[idx] = mpidr;
                    }
                    /* **disabled なコアは起こさない。** 数には入れるが
                     * enable_method は UNKNOWN のままにしておく */
                    if (n->enable_method && !n->is_disabled) {
                        if (str_eq(n->enable_method, "spin-table")) {
                            info->cpu_enable_method[idx] = AARCH64_CPU_ENABLE_SPIN_TABLE;
                        } else if (str_eq(n->enable_method, "psci")) {
                            info->cpu_enable_method[idx] = AARCH64_CPU_ENABLE_PSCI;
                        }
                    }
                    if (n->release_addr_data && n->release_addr_len >= 8U) {
                        info->cpu_release_addr[idx] = aarch64_dtb_read_be64(n->release_addr_data);
                    }
                }
                info->cpu_count++;
            }

            depth--;
            continue;
        }

        if (token == AARCH64_FDT_PROP) {
            uint32_t len, nameoff;
            const uint8_t* data;
            const char* prop_name;

            if (off + 8U > struct_size) break;
            len     = aarch64_dtb_read_be32(struct_base + off);
            nameoff = aarch64_dtb_read_be32(struct_base + off + 4U);
            off += 8U;
            if (off + align_up4(len) > struct_size) break;
            data = struct_base + off;
            prop_name = strings_base + nameoff;

            if (depth <= 0 || depth >= MAX_DEPTH) {
                off += align_up4(len);
                continue;   /* ルート直前や深すぎるノード。読み飛ばす */
            }

            if (str_eq(prop_name, "#address-cells") && len >= 4U) {
                addr_cells[depth] = aarch64_dtb_read_be32(data);
            } else if (str_eq(prop_name, "#size-cells") && len >= 4U) {
                size_cells[depth] = aarch64_dtb_read_be32(data);
            } else if (str_eq(prop_name, "compatible")) {
                const char* c = (const char*)data;
                dtb_node_state_t* n = &nodes[depth];
                if (compatible_has(c, len, "arm,pl011")) n->is_uart = 1;
                if (compatible_has(c, len, "virtio,mmio")) n->is_virtio = 1;
                if (compatible_has(c, len, "arm,armv8-timer") ||
                    compatible_has(c, len, "arm,armv7-timer")) n->is_timer = 1;
                /* GICv2 の名乗り方はいくつかある。QEMU virt は
                 * cortex-a15-gic、**Pi 4 は gic-400** */
                if (compatible_has(c, len, "arm,cortex-a15-gic") ||
                    compatible_has(c, len, "arm,gic-400") ||
                    compatible_has(c, len, "arm,arm11mp-gic")) n->is_gic = 1;
                /* **旧 arasan (brcm,bcm2835-sdhci) は拾わない。**
                 * Pi 4 では mmc@7e300000 が disabled で、生きている
                 * mmcnr@7e300000 は WiFi の SDIO。SD カードは EMMC2 */
                if (compatible_has(c, len, "brcm,bcm2711-emmc2")) n->is_emmc2 = 1;
                /* **Pi 4 でも名乗りは bcm2835-mbox のまま。**世代で変わらない */
                if (compatible_has(c, len, "brcm,bcm2835-mbox")) n->is_mbox = 1;
                /* **ECAM の総称ドライバ名だけを見る。** QEMU virt が名乗るのは
                 * これ。実機の brcm,bcm2711-pcie は設定空間の出し方が違うので
                 * ここでは拾わない (拾うと ECAM として読んで沈黙する) */
                if (compatible_has(c, len, "pci-host-ecam-generic")) n->is_pcie = 1;
                if (compatible_has(c, len, "brcm,bcm2711-pcie")) n->is_pcie_brcm = 1;
            } else if (str_eq(prop_name, "device_type")) {
                if (len >= 7U && str_eq((const char*)data, "memory")) nodes[depth].is_memory = 1;
            } else if (str_eq(prop_name, "reg")) {
                nodes[depth].reg_data = data;
                nodes[depth].reg_len = len;
            } else if (str_eq(prop_name, "enable-method")) {
                nodes[depth].enable_method = (const char*)data;
                nodes[depth].enable_method_len = len;
            } else if (str_eq(prop_name, "cpu-release-addr")) {
                nodes[depth].release_addr_data = data;
                nodes[depth].release_addr_len = len;
            } else if (str_eq(prop_name, "status")) {
                /* "okay" と "ok" だけが有効。他 (disabled / fail / reserved)
                 * は採用しない。**文字列は NUL 終端で入っている** */
                const char* v = (const char*)data;
                if (len > 0 && !str_eq(v, "okay") && !str_eq(v, "ok")) {
                    nodes[depth].is_disabled = 1;
                }
            } else if (str_eq(prop_name, "dma-ranges")) {
                /* **中身は解釈しない。**PCI のものは子が 3 セルで、
                 * 下の汎用の解釈では読めない。使う側で読み直す */
                nodes[depth].dma_ranges_data = data;
                nodes[depth].dma_ranges_len = len;
            } else if (str_eq(prop_name, "ranges")) {
                /* **1 組は「子のアドレス + 親のアドレス + 長さ」。**
                 * セル数は 子=このノードの #address-cells /
                 * 親=親の #address-cells / 長さ=このノードの #size-cells。
                 * **子と親でセル数が違う木がある** (Pi は 1 と 2) ので、
                 * 別々に読むこと。ここを揃えて読むと組の境界がずれる。
                 *
                 * len == 0 の ranges は「そのまま通す」。has_ranges だけ
                 * 立てて組を 0 にしておけば、dtb_translate が素通しにする */
                uint32_t cac = addr_cells[depth];
                uint32_t pac = (depth > 0) ? addr_cells[depth - 1] : addr_cells[0];
                uint32_t csc = size_cells[depth];
                uint32_t stride = (cac + pac + csc) * 4U;
                nodes[depth].has_ranges = 1;
                nodes[depth].range_count = 0;
                nodes[depth].ranges_data = data;
                nodes[depth].ranges_len = len;
                if (stride > 0 && cac <= 2 && pac <= 2 && csc <= 2) {
                    for (uint32_t o = 0;
                         o + stride <= len && nodes[depth].range_count < MAX_RANGES_PER_NODE;
                         o += stride) {
                        const uint8_t* p = data + o;
                        dtb_range_t* r = &nodes[depth].ranges[nodes[depth].range_count++];
                        r->child = (cac == 2) ? aarch64_dtb_read_be64(p)
                                              : (uint64_t)aarch64_dtb_read_be32(p);
                        p += cac * 4U;
                        r->parent = (pac == 2) ? aarch64_dtb_read_be64(p)
                                               : (uint64_t)aarch64_dtb_read_be32(p);
                        p += pac * 4U;
                        r->size = (csc == 2) ? aarch64_dtb_read_be64(p)
                                             : (uint64_t)aarch64_dtb_read_be32(p);
                    }
                }
            } else if (str_eq(prop_name, "interrupts")) {
                nodes[depth].intr_data = data;
                nodes[depth].intr_len = len;
            }

            off += align_up4(len);
            continue;
        }

        if (token == AARCH64_FDT_NOP) continue;
        if (token == AARCH64_FDT_END) break;
        break;      /* 知らないトークン。壊れた DTB を読み進めない */
    }

    if (virtio_count > 0) {
        info->first_virtio_mmio_base = virtio_min;
        info->virtio_mmio_count = virtio_count;
        if (virtio_count > 1) {
            info->virtio_mmio_stride = (virtio_max - virtio_min) / (virtio_count - 1);
        }
        info->flags |= AARCH64_BOOT_FLAG_VIRTIO_FROM_DTB;

        /* **「スロット i の INTID = base + i」が本当に成り立つかを確かめる。**
         * 成り立たない並べ方をしている環境で、勝手に決め打ちしないため。
         * 確かめずに使うと、まったく別のデバイスの割り込みを待つことになる */
        if (virtio_irq_seen == 3 && virtio_count > 1 &&
            virtio_irq_at_max > virtio_irq_at_min &&
            (virtio_irq_at_max - virtio_irq_at_min) == (virtio_count - 1)) {
            info->virtio_mmio_irq_base = virtio_irq_at_min;
            info->flags |= AARCH64_BOOT_FLAG_VIRTIO_IRQ_OK;
        }
    }
}
