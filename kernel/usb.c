#include <stdint.h>
#include "usb.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"

/* **1ms 待ちの時刻源。ここが唯一のアーキ依存だった。**
 * x86 は LAPIC のタイマ、aarch64 は generic timer から取る。
 * 中身 (xHCI のリングやスロット操作) は両方で同じものが動く */
#if defined(__x86_64__)
#include "lapic.h"
#define USB_NOW_MS() lapic_get_ticks_ms()
#define USB_CPU_RELAX() __asm__ volatile("pause")
#else
uint64_t arch_time_now_ms(void);
#define USB_NOW_MS() arch_time_now_ms()
/* aarch64 に pause は無い。**同じ役目は yield** — 待ちループで
 * 他のハードウェアスレッドに譲るヒント */
#define USB_CPU_RELAX() __asm__ volatile("yield")
#endif

/* ---- メモリバリア ---------------------------------------------------------
 *
 * **リングは Normal-NC、レジスタは Device-nGnRnE。この 2 つの間には
 * 順序の保証が無い。**
 *
 * aarch64 では Device-nGnRnE 同士は順序が守られるが、Normal-NC の書き込みと
 * Device の書き込みの間は**並べ替えられる。**つまり
 *
 *     TRB を書く (Normal-NC)  →  ドアベルを叩く (Device)
 *
 * の 2 つが逆順にデバイスへ届き得る。**ドアベルが先に着くと、xHC は
 * まだ書かれていない TRB を読む。**「たまに動く」という最悪の壊れ方になる。
 *
 * イベント側も同じで、**イベントを読み終える前に ERDP が進むと**
 * xHC はその TRB を再利用してよいと判断する。
 *
 * x86 は書き込みが順序を守る (TSO) ので本来は不要だが、
 * **コンパイラの並べ替えは止める必要がある**ので mfence を置く */
#if defined(__x86_64__)
#define USB_MB() __asm__ volatile("mfence" ::: "memory")
#else
#define USB_MB() __asm__ volatile("dsb sy" ::: "memory")
#endif

void puts(const char* s);
void puthex(uint64_t v);

/* **人が読む数は 10 進で出す。**puthex は 64bit を丸ごと出すので、
 * ポート番号のような小さい数が 0x0000000000000005 になって読みにくい。
 * 実機のログを読むのが目的なので、そこは削る */
/* 1 バイトを 2 桁で出す。記述子のダンプ用 */
static void puthex_byte(uint8_t v) {
    static const char hex[] = "0123456789abcdef";
    char out[3];
    out[0] = hex[(v >> 4) & 0xF];
    out[1] = hex[v & 0xF];
    out[2] = 0;
    puts(out);
}

static void putdec(uint64_t v) {
    char rev[21];
    char out[21];
    int i = 0, j = 0;
    if (v == 0) { puts("0"); return; }
    while (v && i < 20) { rev[i++] = (char)('0' + (v % 10U)); v /= 10U; }
    while (i) out[j++] = rev[--i];
    out[j] = 0;
    puts(out);
}

/* **アーキ固有の経路で見つけた xHCI の MMIO 番地。**
 * 0 = 持っていない (PCI から探す)。既定は弱いシンボルで 0 を返す
 * (kernel/fs.c の arch_console_echo_enabled と同じ形) */
__attribute__((weak))
uint64_t usb_arch_xhci_mmio(void) { return 0; }

/* ---- DMA の番地変換 -------------------------------------------------------
 *
 * **デバイスから見た番地と CPU の物理が一致するとは限らない。**
 *
 * Raspberry Pi 4 では PCIe の内向き窓が
 *
 *     PCI 0x4_00000000  <->  CPU 物理 0x0    (3GB)
 *
 * になっている (実機の dma-ranges と Linux の dmesg で確認)。**リングや
 * バッファの番地をそのまま渡すと、デバイスは別の場所を読みに行く。**
 * 「初期化は通るのに転送だけ動かない」という最悪の形で嵌まる。
 *
 * ---- どう扱うか ------------------------------------------------------------
 *
 * **確保した時点でデバイス視点の番地にしてしまう。** g_*_phys に入るのは
 * 全部「デバイスから見た番地」で、CPU が中身を触るときだけ引き算する。
 *
 * こうすると、デバイスに番地を渡している 30 か所以上に手を入れずに済む。
 * **触るのは確保 (usb_alloc_dma) と参照 (USB_VIRT) の 2 か所だけ**で、
 * 数で検証できる。
 *
 * **MMIO には使わない。** xHCI のレジスタは CPU の物理番地で、
 * DMA の窓とは無関係 */
__attribute__((weak))
uint64_t usb_arch_dma_offset(void) { return 0; }

/* **DMA に使うメモリを arch から取る。**
 *
 * PCIe が CPU のキャッシュとコヒーレントでない機械がある。Raspberry Pi 4
 * (BCM2711) がそうで、実機の DTB の pcie ノードに `dma-coherent` が無い。
 * そこで pmm から取った普通の (Normal-WB の) ページにリングを置くと:
 *
 *   - こちらが書いたコマンド TRB がキャッシュに留まり、デバイスは
 *     DRAM の古い内容を読む
 *   - デバイスが書いたイベントを、こちらはキャッシュから読んで見落とす
 *
 * **QEMU では絶対に出ない** (TCG はキャッシュを模擬しない)。実機で
 * 「MMIO は読めるのにイベントが 1 つも来ない」形でだけ現れる。
 *
 * 0 を返す arch では従来どおり pmm_alloc を使う (コヒーレントな機械)。
 * 返すのは **物理番地**。中身は 0 になっていること */
__attribute__((weak))
uint64_t usb_arch_dma_alloc_phys(size_t pages) { (void)pages; return 0; }

static uint64_t g_usb_dma_offset;

/* 確保してデバイス視点の番地を返す */
static void* usb_alloc_dma(size_t pages) {
    uint64_t nc = usb_arch_dma_alloc_phys(pages);
    void* p = nc ? (void*)(uintptr_t)nc : pmm_alloc(pages);
    if (!p) return 0;
    return (void*)((uint64_t)(uintptr_t)p + g_usb_dma_offset);
}

/* デバイス視点の番地の中身を CPU から触る */
#define PHYS_TO_VIRT_MMIO(pa) PHYS_TO_VIRT(pa)
#define USB_VIRT(dma) PHYS_TO_VIRT((void*)((uint64_t)(uintptr_t)(dma) - g_usb_dma_offset))

static int g_usb_ready = 0;
static int g_usb_mass_ready = 0;
static int g_xhci_rings_ready = 0;
static int g_xhci_cmd_ready = 0;
static int g_xhci_addr_ready = 0;
static int g_xhci_cfg_ready = 0;
static uint8_t g_xhci_slot_id = 0;
static uint8_t g_xhci_port_id = 0;
static int g_usb_desc_ready = 0;
static int g_usb_cfg_desc_ready = 0;
static int g_usb_msc_if_ready = 0;
static int g_usb_msc_bot_ready = 0;
static int g_usb_msc_inquiry_ok = 0;
static int g_usb_msc_capacity_ok = 0;
static uint16_t g_usb_vid = 0;
static uint16_t g_usb_pid = 0;
static uint8_t g_usb_dev_class = 0;
static uint8_t g_usb_dev_subclass = 0;
static uint8_t g_usb_dev_proto = 0;
static uint8_t g_usb_port_speed = 0;
static uint8_t g_usb_msc_if_class = 0;
static uint8_t g_usb_msc_if_subclass = 0;
static uint8_t g_usb_msc_if_proto = 0;
static uint8_t g_usb_cfg_value = 0;
static uint8_t g_usb_msc_if_number = 0;
/* ---- HID (キーボード) -----------------------------------------------------
 *
 * **既存の経路は大容量記憶装置しか探していなかった。**キーボードは
 * インターフェースのクラスが 0x03 (HID) で、**割り込み IN エンドポイント**を
 * 1 本持つ。bulk と並べて拾えるようにする。
 *
 * boot protocol を使うので、レポートの中身は 8 バイト固定:
 *   [0] 修飾キー  [1] 予約  [2..7] 同時に押されているキーのコード
 * **HID レポートデスクリプタを解釈しなくてよい**のが boot protocol の利点 */
static int      g_usb_hid_if_ready = 0;
static uint8_t  g_usb_hid_if_number = 0;
static uint8_t  g_usb_hid_if_proto = 0;   /* 1 = キーボード、2 = マウス */
static uint8_t  g_usb_int_in_ep = 0;
static uint16_t g_usb_int_in_mps = 0;
static uint8_t  g_usb_int_in_interval = 0;
static uint8_t  g_usb_int_in_dci = 0;

static uint64_t g_int_ring_phys = 0;
static uint64_t g_int_buf_phys = 0;
static uint32_t g_int_enqueue_idx = 0;
static uint32_t g_int_cycle = 1;
static int      g_usb_kbd_ready = 0;

static uint8_t g_usb_bulk_out_ep = 0;
static uint8_t g_usb_bulk_in_ep = 0;
static uint16_t g_usb_ep0_mps = 64;
static uint16_t g_usb_bulk_out_mps = 0;
static uint16_t g_usb_bulk_in_mps = 0;
static uint8_t g_usb_bulk_out_dci = 0;
static uint8_t g_usb_bulk_in_dci = 0;
static uint32_t g_usb_block_count = 0;
static uint32_t g_usb_block_size = 0;
static uint64_t g_xhci_mmio = 0;
static uint8_t g_xhci_max_ports = 0;
/* **ポートごとの USB のメジャー番号** (2 = USB2 / 3 = USB3)。0 = 不明。
 *
 * **MaxPorts から決め打ちはできない。**Raspberry Pi 4 の VL805 は
 * USB2 のポートと USB3 のポートが 1 本の番号空間に混ざって並んでいて、
 * **どれがどちらかは xECP の Supported Protocol Capability にしか書いていない。**
 * リセットの手順が USB2 (PR) と USB3 (WPR) で違うので、ここを間違えると
 * ポートが有効にならない */
#define XHCI_MAX_PORTS_TRACKED 32U
static uint8_t g_port_major[XHCI_MAX_PORTS_TRACKED];
static uint32_t g_xhci_dboff = 0;
static uint32_t g_xhci_rtsoff = 0;

// xHCI runtime objects (single segment per ring for bring-up).
static uint64_t g_dcbaap_phys = 0;
static uint64_t g_cmd_ring_phys = 0;
static uint64_t g_erst_phys = 0;
static uint64_t g_event_ring_phys = 0;
static uint64_t g_scratchpad_array_phys = 0;
static uint32_t g_scratchpad_count = 0;
static uint32_t g_xhci_ctx_size = 32;
static volatile uint8_t* g_cap_regs = 0;
static volatile uint8_t* g_op_regs = 0;
static volatile uint8_t* g_db_regs = 0;
static volatile uint8_t* g_rt_regs = 0;
static uint32_t g_cmd_enqueue_idx = 0;
static uint32_t g_cmd_cycle = 1;
/* イベントリングは 4KB 1 面 = 16 バイト * 256。**ERST に書くサイズと
 * 一周の判定は必ず同じ値でないといけない**ので定数を 1 つにする */
#define XHCI_EVT_RING_TRBS 256U
static uint32_t g_evt_dequeue_idx = 0;
static uint32_t g_evt_cycle = 1;
static uint64_t g_input_ctx_phys = 0;
static uint64_t g_output_ctx_phys = 0;
static uint64_t g_ep0_ring_phys = 0;
static uint64_t g_ep0_buf_phys = 0;
static uint64_t g_ep0_cfg_buf_phys = 0;
static uint32_t g_ep0_enqueue_idx = 0;
static uint32_t g_ep0_cycle = 1;
static uint64_t g_bulk_out_ring_phys = 0;
static uint64_t g_bulk_in_ring_phys = 0;
static uint64_t g_bulk_buf_phys = 0;
static uint32_t g_bulk_out_enqueue_idx = 0;
static uint32_t g_bulk_out_cycle = 1;
static uint32_t g_bulk_in_enqueue_idx = 0;
static uint32_t g_bulk_in_cycle = 1;
static uint32_t g_usb_msc_tag = 1;

#define USB_BULK_BUF_PAGES 2
#define USB_BULK_BUF_SIZE (USB_BULK_BUF_PAGES * PAGE_SIZE)
#define USB_BULK_CSW_OFFSET 512U
#define USB_BULK_DATA_OFFSET 1024U

struct usb_msc_cbw {
    uint32_t sig;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} __attribute__((packed));

struct usb_msc_csw {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed));

static inline uint32_t mmio_read32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void mmio_write32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

static inline void mmio_write64(volatile uint8_t* base, uint32_t off, uint64_t v) {
    mmio_write32(base, off, (uint32_t)(v & 0xFFFFFFFFU));
    mmio_write32(base, off + 4, (uint32_t)(v >> 32));
}

static void memzero(void* p, uint64_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void memcopy(void* dst, const void* src, uint64_t n) {
    volatile uint8_t* d = (volatile uint8_t*)dst;
    const volatile uint8_t* s = (const volatile uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static uint8_t xhci_dci_from_epaddr(uint8_t ep_addr) {
    uint8_t ep_num = ep_addr & 0x0F;
    if (ep_num == 0) return 1;
    return (uint8_t)(ep_num * 2U + ((ep_addr & 0x80U) ? 1U : 0U));
}

static uint32_t xhci_ctx_offset_dw(uint8_t dci) {
    uint32_t stride_dw = g_xhci_ctx_size / 4U;
    return stride_dw * (1U + (uint32_t)dci);
}

static void xhci_ring_reset(uint64_t ring_phys) {
    volatile uint32_t* ring = (volatile uint32_t*)USB_VIRT((void*)ring_phys);
    memzero((void*)ring, PAGE_SIZE);
    ring[255 * 4 + 0] = (uint32_t)(ring_phys & 0xFFFFFFFFU);
    ring[255 * 4 + 1] = (uint32_t)(ring_phys >> 32);
    ring[255 * 4 + 2] = 0;
    ring[255 * 4 + 3] = (6U << 10) | (1U << 1) | 1U;
}

static void xhci_ring_enqueue_advance(uint32_t* idx, uint32_t* cycle, uint32_t count) {
    while (count--) {
        (*idx)++;
        if (*idx == 255) {
            *idx = 0;
            *cycle ^= 1U;
        }
    }
}

static int xhci_wait_bits(volatile uint8_t* op, uint32_t off, uint32_t mask, uint32_t expected, uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++) {
        if ((mmio_read32(op, off) & mask) == expected) return 0;
    }
    return -1;
}

/* ---- イベントリングから 1 つ取り出す ---------------------------------------
 *
 * **2026-08-16 に実機で嵌まったのはここ。**
 *
 * ERDP は「こちらがどこまで消費したか」を xHC に伝える**消費側**の指標で、
 * xHC の書き込み位置ではない。書き込み位置は xHC が内部に持っていて、
 * **こちらから巻き戻す手段は無い。**
 *
 * それを知らずに、コマンドを出すたびに ERDP を先頭へ書き戻してリングを
 * memzero していた。ポートに何か挿さっていると起動時に Port Status Change
 * Event が index 0 に入るので、**No-Op の完了は index 1 に書かれる。**
 * こちらは永久に index 0 (0 のまま) を見続けて、必ずタイムアウトする。
 * memzero は xHC が DMA 書き込み中のメモリを消す競合でもあった。
 *
 * **判定はサイクルビットだけで行う。**取り出し位置は進めるだけ、一周したら
 * サイクルを反転。サイクルが合わなければリングは空。
 *
 * 戻り値: 1 = 取れた / 0 = 空 */
static int xhci_evt_dequeue(uint32_t out[4]) {
    if (!g_rt_regs || g_event_ring_phys == 0) return 0;

    volatile uint32_t* trb =
        (volatile uint32_t*)USB_VIRT((void*)g_event_ring_phys) + g_evt_dequeue_idx * 4U;

    /* **サイクルビットを先に読む。**これが一致して初めて、残り 3 ワードも
     * 書き終わっていると言える */
    uint32_t d3 = trb[3];
    if ((d3 & 1U) != g_evt_cycle) return 0;
    USB_MB();

    out[0] = trb[0];
    out[1] = trb[1];
    out[2] = trb[2];
    out[3] = d3;

    g_evt_dequeue_idx++;
    if (g_evt_dequeue_idx >= XHCI_EVT_RING_TRBS) {
        g_evt_dequeue_idx = 0;
        g_evt_cycle ^= 1U;
    }

    /* **読み終えてから ERDP を進める。**逆になると xHC はこちらがまだ
     * 読んでいない TRB を再利用できると判断する */
    USB_MB();

    /* **EHB (bit 3) は RW1C。**1 を書いて落とさないと立ったままになり、
     * 次のイベントで割り込みが上がらなくなる (実機で立ちっぱなしを観測) */
    mmio_write64(g_rt_regs, 0x20 + 0x18,
                 ((g_event_ring_phys + (uint64_t)g_evt_dequeue_idx * 16ULL) & ~0xFULL)
                     | (1ULL << 3));
    return 1;
}

/* **滞留しているイベントをサイクル不一致まで捨てる。**
 * ポートに何か挿さっていれば Port Status Change Event が必ず入っていて、
 * 残したままだとこちらの取り出し位置がずれ続ける。戻り値は捨てた数 */
static uint32_t xhci_evt_drain(void) {
    uint32_t d[4];
    uint32_t n = 0;
    while (n < XHCI_EVT_RING_TRBS && xhci_evt_dequeue(d)) n++;
    return n;
}

static int xhci_poll_cmd_completion(uint64_t* out_cmd_ptr, uint8_t* out_cc, uint8_t* out_slot_id, uint32_t loops) {
    if (!g_rt_regs || !out_cmd_ptr || !out_cc || !out_slot_id) return -1;

    for (uint32_t i = 0; i < loops; i++) {
        uint32_t d[4];
        if (!xhci_evt_dequeue(d)) continue;

        uint32_t type = (d[3] >> 10) & 0x3F;
        if (type == 33) { // Command Completion Event
            *out_cmd_ptr = ((uint64_t)d[1] << 32) | d[0];
            *out_cc = (uint8_t)((d[2] >> 24) & 0xFF);
            *out_slot_id = (uint8_t)((d[3] >> 24) & 0xFF);
        } else {
            *out_cmd_ptr = 0;
            *out_cc = 0xFF;
            *out_slot_id = 0;
        }
        return (type == 33) ? 0 : 1;
    }

    return -1;
}

static uint64_t xhci_cmd_submit(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3_no_cycle) {
    if (!g_db_regs || g_cmd_ring_phys == 0) return 0;
    volatile uint32_t* cmd_ring = (volatile uint32_t*)USB_VIRT((void*)g_cmd_ring_phys);

    uint32_t idx = g_cmd_enqueue_idx;
    volatile uint32_t* trb = &cmd_ring[idx * 4];
    trb[0] = d0;
    trb[1] = d1;
    trb[2] = d2;
    trb[3] = d3_no_cycle | (g_cmd_cycle & 1U);

    uint64_t trb_phys = g_cmd_ring_phys + (uint64_t)idx * 16ULL;

    g_cmd_enqueue_idx++;
    if (g_cmd_enqueue_idx == 255) {
        g_cmd_enqueue_idx = 0;
        g_cmd_cycle ^= 1U;
    }

    /* **TRB を書き終えてからドアベルを叩く。**コンパイラだけでなく
     * ハードウェアの並べ替えも止める必要がある (Normal-NC と Device の間に
     * 順序の保証は無い) */
    USB_MB();

    // Ring command doorbell (DB0).
    mmio_write32(g_db_regs, 0x00, 0);
    return trb_phys;
}

static int xhci_cmd_noop(void) {
    /* **リングを消したり ERDP を巻き戻したりしない。**
     * 代わりに滞留しているイベントを消費して位置を合わせる。
     * 理由は xhci_evt_dequeue の注釈にある */
    uint32_t stale = xhci_evt_drain();
    if (stale) {
        puts("[usb] 滞留イベントを ");
        puthex(stale);
        puts(" 個捨てた\r\n");
    }

    uint64_t cmd_ptr = xhci_cmd_submit(0, 0, 0, (23U << 10)); // No-Op Command TRB
    if (!cmd_ptr) return -1;

    for (uint32_t i = 0; i < 8000000; i++) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 1);
        if (r < 0) continue;
        if (r == 0 && ((ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL))) {
            (void)slot;
            return (cc == 1) ? 0 : -2; // 1: Success
        }
        if (r == 0 && cc == 1) {
            puts("[usb] cmd completion ptr mismatch sent=0x");
            puthex(cmd_ptr);
            puts(" got=0x");
            puthex(ev_cmd_ptr);
            puts("\r\n");
            return 0;
        }
    }

    if (g_op_regs && g_rt_regs) {
        uint32_t usbcmd = mmio_read32(g_op_regs, 0x00);
        uint32_t usbsts = mmio_read32(g_op_regs, 0x04);
        uint32_t iman = mmio_read32(g_rt_regs, 0x20 + 0x00);
        uint64_t erdp = ((uint64_t)mmio_read32(g_rt_regs, 0x20 + 0x1C) << 32) |
                        (uint64_t)mmio_read32(g_rt_regs, 0x20 + 0x18);
        volatile uint32_t* ev0 = (volatile uint32_t*)USB_VIRT((void*)g_event_ring_phys);
        puts("[usb] noop timeout usbcmd=0x");
        puthex(usbcmd);
        puts(" usbsts=0x");
        puthex(usbsts);
        puts(" iman=0x");
        puthex(iman);
        puts(" erdp=0x");
        puthex(erdp);
        puts("\r\n");
        /* ★ **先頭 1 つだけ見ていては分からない。**
         *
         * ERDP はこちらの取り出し位置であって、**xHC の書き込み位置は
         * 別に進んでいる。**起動直後に滞留していた Port Status Change
         * Event のぶんだけずれていると、こちらが見ている TRB は
         * 永久に 0 のままになる (2026-08-16 実機)。
         *
         * **0 でない TRB だけを番号つきで出す。**どこに書かれたかが
         * 一目で分かる */
        {
            uint32_t i, shown = 0;
            for (i = 0; i < 16U; i++) {
                volatile uint32_t* t = ev0 + i * 4U;
                if (!t[0] && !t[1] && !t[2] && !t[3]) continue;
                puts("[usb] evt[");
                puthex(i);
                puts("] d0=0x");
                puthex(t[0]);
                puts(" d1=0x");
                puthex(t[1]);
                puts(" d2=0x");
                puthex(t[2]);
                puts(" d3=0x");
                puthex(t[3]);
                puts("\r\n");
                shown++;
            }
            if (!shown) puts("[usb] evt[0..15] すべて 0 (1 つも書かれていない)\r\n");
        }
    }
    return -3;
}

static int xhci_cmd_enable_slot(uint8_t* out_slot_id) {
    if (out_slot_id) *out_slot_id = 0;
    uint64_t cmd_ptr = xhci_cmd_submit(0, 0, 0, (9U << 10)); // Enable Slot Command
    if (!cmd_ptr) return -1;

    for (uint32_t i = 0; i < 8000000; i++) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 1);
        if (r < 0) continue;
        if (r == 0 && (ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL)) {
            if (cc != 1) return -2;
            if (out_slot_id) *out_slot_id = slot;
            return (slot != 0) ? 0 : -3;
        }
    }
    return -4;
}

static int xhci_wait_cmd_completion(uint64_t cmd_ptr, uint8_t slot_expect, uint8_t* out_cc, uint8_t* out_slot) {
    for (uint32_t i = 0; i < 8000000; i++) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 1);
        if (r < 0) continue;
        if (r == 0 && (ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL)) {
            if (out_cc) *out_cc = cc;
            if (out_slot) *out_slot = slot;
            if (slot_expect && slot != slot_expect) return -2;
            return 0;
        }
    }
    return -1;
}

/* **イベントが指す TRB の番地も返す。**
 * これが無いと Setup / Data / Status のどの段で落ちたかが分からない
 * (2026-08-18 codex 相談) */
static uint64_t g_last_evt_trb = 0;

static int xhci_poll_transfer_event(uint8_t slot_expect, uint8_t ep_expect, uint64_t trb_expect,
                                    uint32_t loops, uint8_t* out_cc, uint32_t* out_residual) {
    if (!g_rt_regs) return -1;
    for (uint32_t i = 0; i < loops; i++) {
        uint32_t d[4];
        if (!xhci_evt_dequeue(d)) continue;

        uint32_t type = (d[3] >> 10) & 0x3F;
        uint8_t slot = (uint8_t)((d[3] >> 24) & 0xFF);
        uint8_t ep = (uint8_t)((d[3] >> 16) & 0x1F);
        uint64_t trb_ptr = (((uint64_t)d[1] << 32) | d[0]) & ~0xFULL;

        if (type != 32) continue; // Transfer Event
        if (slot_expect && slot != slot_expect) continue;
        if (ep_expect && ep != ep_expect) continue;
        if (trb_expect && trb_ptr != (trb_expect & ~0xFULL)) continue;

        if (out_cc) *out_cc = (uint8_t)((d[2] >> 24) & 0xFF);
        if (out_residual) *out_residual = (d[2] & 0x00FFFFFFU);
        g_last_evt_trb = trb_ptr;
        return 0;
    }
    return -1;
}

/* ---- Address Device ------------------------------------------------------
 *
 * ルートポートに直付けされたデバイスと、**ハブの先のデバイス**の両方を扱う。
 *
 *   route      : Route String。**ハブの何番ポートの先か**を 4bit ずつ並べる。
 *                ルート直下なら 0
 *   speed      : 0 なら PORTSC から取る。ハブの先はルートの PORTSC を見ても
 *                **ハブ自身の速度しか分からない**ので、呼ぶ側が渡す
 *   tt_hub_slot / tt_port :
 *                **低速・全速のデバイスが高速ハブの先にいるとき必須。**
 *                xHC はこの 2 つを見て Transaction Translator を選ぶ。
 *                入れ忘れると Address Device は通るのに転送だけ落ちる */
static int xhci_cmd_address_device_full(uint8_t slot_id, uint8_t port_id, uint32_t route,
                                        uint32_t speed_in, uint8_t tt_hub_slot, uint8_t tt_port,
                                        uint32_t mps_override) {
    if (slot_id == 0 || port_id == 0) return -1;
    if (g_xhci_ctx_size != 32 && g_xhci_ctx_size != 64) return -2;

    void* in_ctx = usb_alloc_dma(1);
    void* out_ctx = usb_alloc_dma(1);
    void* ep0_ring = usb_alloc_dma(1);
    if (!in_ctx || !out_ctx || !ep0_ring) return -3;

    g_input_ctx_phys = (uint64_t)in_ctx;
    g_output_ctx_phys = (uint64_t)out_ctx;
    g_ep0_ring_phys = (uint64_t)ep0_ring;
    if (!g_ep0_buf_phys) {
        void* ep0_buf = usb_alloc_dma(1);
        if (!ep0_buf) return -3;
        memzero(USB_VIRT(ep0_buf), PAGE_SIZE);
        g_ep0_buf_phys = (uint64_t)ep0_buf;
    }

    memzero(USB_VIRT(in_ctx), PAGE_SIZE);
    memzero(USB_VIRT(out_ctx), PAGE_SIZE);
    memzero(USB_VIRT(ep0_ring), PAGE_SIZE);

    // EP0 transfer ring: set Link TRB at tail to make a cycle ring.
    volatile uint32_t* ep = (volatile uint32_t*)USB_VIRT(ep0_ring);
    ep[255 * 4 + 0] = (uint32_t)(g_ep0_ring_phys & 0xFFFFFFFFU);
    ep[255 * 4 + 1] = (uint32_t)(g_ep0_ring_phys >> 32);
    ep[255 * 4 + 2] = 0;
    ep[255 * 4 + 3] = (6U << 10) | (1U << 1) | 1U;
    g_ep0_enqueue_idx = 0;
    g_ep0_cycle = 1;

    // DCBAA[slot_id] -> output device context.
    volatile uint64_t* dcbaa = (volatile uint64_t*)USB_VIRT((void*)g_dcbaap_phys);
    dcbaa[slot_id] = g_output_ctx_phys;

    volatile uint32_t* ic = (volatile uint32_t*)USB_VIRT(in_ctx);
    uint32_t stride_dw = g_xhci_ctx_size / 4U;
    // Input Control Context: Add Slot + EP0 context.
    ic[1] = 0x00000003U;

    // Slot Context (offset = 1 context).
    uint32_t slot_off = stride_dw * 1U;
    uint32_t speed = speed_in;
    if (speed == 0) {
        uint32_t portsc = mmio_read32(g_op_regs, 0x400 + ((uint32_t)port_id - 1U) * 0x10);
        speed = (portsc >> 10) & 0xF;
    }
    if (speed == 0) speed = 3; // fall back to high-speed profile for qemu usb-storage path
    g_usb_port_speed = (uint8_t)speed;
    /* **EP0 の最大パケット長は速度で決まる。**低速/全速で 64 を入れると
     * 記述子の読み出しが通らない。低速は 8 しか許されない */
    g_usb_ep0_mps = mps_override ? (uint16_t)mps_override
                                 : ((speed >= 4) ? 512U : ((speed == 3) ? 64U : 8U));
    ic[slot_off + 0] = (route & 0xFFFFFU) | (speed << 20) | (1U << 27); // ROUTE + SPEED + CONTEXT_ENTRIES=1
    ic[slot_off + 1] = ((uint32_t)port_id << 16);          // ROOT_HUB_PORT_NUM
    ic[slot_off + 2] = (uint32_t)tt_hub_slot | ((uint32_t)tt_port << 8);
    ic[slot_off + 3] = 0;

    // EP0 Context (offset = 2 contexts, DCI=1).
    uint32_t ep0_off = stride_dw * 2U;
    ic[ep0_off + 0] = (0U << 16);                          // EP_STATE disabled for input
    /* EP_TYPE=Control(4) は bits[5:3]。**CErr (bits[2:1]) に 3 を入れる。**
     * 0 のままだと再試行なしで、1 回の取りこぼしがそのまま失敗になる */
    ic[ep0_off + 1] = (4U << 3) | (3U << 1) | ((uint32_t)g_usb_ep0_mps << 16);
    ic[ep0_off + 2] = (uint32_t)((g_ep0_ring_phys & ~0xFULL) | 1U); // TR Dequeue + DCS
    ic[ep0_off + 3] = (uint32_t)(g_ep0_ring_phys >> 32);
    /* **Average TRB Length。制御エンドポイントは 8 が必須で、0 は不可。**
     * 仕様上 0 以外が要求される欄で、Linux も制御 EP には 8 を入れている。
     * QEMU は見ないが実機は見る (2026-08-18 codex 相談) */
    ic[ep0_off + 4] = 8;

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (11U << 10) | ((uint32_t)slot_id << 24) // Address Device Command
    );
    if (!cmd_ptr) return -4;

    uint8_t cc = 0xFF;
    uint8_t slot = 0;
    int w = xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot);
    if (w < 0) return -5;
    (void)slot;
    if (cc != 1) {
        /* **cc が無いと原因が絞れない。**
         * 19 = Context State Error (そのスロットの状態からは出せない命令)
         * 17 = Parameter Error (文脈の中身が不正) */
        puts("[usb] address device cc=");
        puthex(cc);
        puts("\r\n");
        return -6;
    }
    return 0;
}

/* スロットを手放す。**やり直すときは作り直すのが一番確実。**
 * Address Device は Configured 状態のスロットには出せない */
__attribute__((unused))
static int xhci_cmd_disable_slot(uint8_t slot_id) {
    uint64_t cmd_ptr;
    uint8_t cc = 0xFF, slot = 0;
    if (slot_id == 0) return -1;
    cmd_ptr = xhci_cmd_submit(0, 0, 0, (10U << 10) | ((uint32_t)slot_id << 24));
    if (!cmd_ptr) return -2;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) return -3;
    return (cc == 1) ? 0 : -4;
}

static int xhci_cmd_address_device(uint8_t slot_id, uint8_t port_id) {
    return xhci_cmd_address_device_full(slot_id, port_id, 0, 0, 0, 0, 0);
}

static int xhci_cmd_configure_endpoint(uint8_t slot_id) {
    if (slot_id == 0 || g_input_ctx_phys == 0) return -1;

    volatile uint32_t* ic = (volatile uint32_t*)USB_VIRT((void*)g_input_ctx_phys);
    // For Configure Endpoint, A1 must stay clear; EP0 context does not apply.
    ic[0] = 0;           // Drop flags
    ic[1] = 0x00000001U; // Add flags: slot only

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (12U << 10) | ((uint32_t)slot_id << 24) // Configure Endpoint Command
    );
    if (!cmd_ptr) return -2;

    uint8_t cc = 0xFF;
    uint8_t slot = 0;
    int w = xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot);
    if (w < 0) return -3;
    (void)slot;
    return (cc == 1) ? 0 : -4;
}

/* **EP0 の最大パケット長を後から入れ替える。**
 *
 * 全速のデバイスは bMaxPacketSize0 が 8/16/32/64 のどれかで、**繋いでみるまで
 * 分からない。**こちらが 8 と思っているのに相手が 64 で返すと、xHC は
 * 「MPS より長いパケットが来た」と見て Babble (cc=3) を上げる。
 *
 * 手順は仕様どおり: **8 バイトだけ読む → bMaxPacketSize0 を見る →
 * Evaluate Context で入れ替える → 18 バイト読む。**
 *
 * 高速は 64 しか許されないので、この問題は全速・低速でしか出ない。
 * ルートポート直結の相手は高速だったので、**ハブの先に行くまで露見しなかった** */
/* STALL からの復帰は下で定義する (コマンドリングのヘルパを使うため) */
static int xhci_ep0_recover_stall(uint8_t slot_id);

/* **空回しではなく時刻で待つ。**空回しの回数は最適化と CPU の速さで変わるので、
 * 「10ms 待った」ことの保証にならない */
static void usb_delay_ms(uint32_t ms) {
    uint64_t t0 = USB_NOW_MS();
    while ((USB_NOW_MS() - t0) < (uint64_t)ms) USB_CPU_RELAX();
}

static int xhci_cmd_evaluate_context_mps(uint8_t slot_id, uint32_t mps) {
    volatile uint32_t* ic;
    uint32_t ep0_off = (g_xhci_ctx_size / 4U) * 2U;
    uint64_t cmd_ptr;
    uint8_t cc = 0xFF, slot = 0;

    if (slot_id == 0 || g_input_ctx_phys == 0) return -1;
    ic = (volatile uint32_t*)USB_VIRT((void*)g_input_ctx_phys);
    ic[0] = 0;
    ic[1] = 0x00000002U; /* A1 のみ = EP0 の文脈だけ評価させる */
    ic[ep0_off + 1] = (ic[ep0_off + 1] & 0x0000FFFFU) | (mps << 16);
    ic[ep0_off + 4] = (ic[ep0_off + 4] & 0xFFFF0000U) | 8U; /* Average TRB Length */

    cmd_ptr = xhci_cmd_submit((uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
                              (uint32_t)(g_input_ctx_phys >> 32), 0,
                              (13U << 10) | ((uint32_t)slot_id << 24)); /* Evaluate Context */
    if (!cmd_ptr) return -2;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) return -3;
    return (cc == 1) ? 0 : -4;
}

static int xhci_ep0_get_device_descriptor(uint8_t slot_id) {
    if (slot_id == 0 || g_ep0_ring_phys == 0 || g_ep0_buf_phys == 0) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)USB_VIRT((void*)g_ep0_ring_phys);
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cycle = g_ep0_cycle & 1U;

    // USB Device Descriptor request (18 bytes):
    // bmRequestType=0x80, bRequest=GET_DESCRIPTOR(6), wValue=0x0100, wIndex=0, wLength=18
    uint64_t setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0100 << 16;
    setup |= (uint64_t)0 << 32;
    setup |= (uint64_t)18 << 48;

    // Setup Stage TRB (TRT=IN => 3)
    ep[idx * 4 + 0] = (uint32_t)(setup & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(setup >> 32);
    ep[idx * 4 + 2] = 8; // setup packet is 8 bytes
    ep[idx * 4 + 3] = (2U << 10) | (1U << 6) | (3U << 16) | (1U << 4) | cycle; // Setup, IDT, TRT=IN, CHAIN

    // Data Stage TRB (IN, 18 bytes)
    idx++;
    ep[idx * 4 + 0] = (uint32_t)(g_ep0_buf_phys & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(g_ep0_buf_phys >> 32);
    ep[idx * 4 + 2] = 18;
    ep[idx * 4 + 3] = (3U << 10) | (1U << 16) | (1U << 4) | cycle; // Data IN, CHAIN

    // Status Stage TRB (OUT status, IOC)
    idx++;
    ep[idx * 4 + 0] = 0;
    ep[idx * 4 + 1] = 0;
    ep[idx * 4 + 2] = 0;
    ep[idx * 4 + 3] = (4U << 10) | (1U << 5) | cycle; // Status, IOC

    USB_MB(); /* TRB を書き終えてからドアベル */
    // Ring EP0 doorbell (DB[slot], target endpoint 1)
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    xhci_ring_enqueue_advance(&g_ep0_enqueue_idx, &g_ep0_cycle, 3);

    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        /* **落ちた理由を出す。**cc が無いと推測しかできない。
         * 3 = Babble (相手のパケットがこちらの MPS より長い)
         * 4 = USB Transaction Error / 6 = STALL */
        puts("[usb] device desc cc=");
        puthex(cc);
        puts(" mps=");
        putdec(g_usb_ep0_mps);
        puts(" speed=");
        putdec(g_usb_port_speed);
        puts("\r\n");
        (void)xhci_ep0_recover_stall(slot_id);
        return -3;
    }

    volatile uint8_t* d = (volatile uint8_t*)USB_VIRT((void*)g_ep0_buf_phys);
    if (d[1] != 1) return -4; // bDescriptorType must be DEVICE

    g_usb_vid = (uint16_t)d[8] | ((uint16_t)d[9] << 8);
    g_usb_pid = (uint16_t)d[10] | ((uint16_t)d[11] << 8);
    g_usb_dev_class = d[4];
    g_usb_dev_subclass = d[5];
    g_usb_dev_proto = d[6];
    g_usb_desc_ready = 1;
    (void)residual;
    return 0;
}

/* **1 回の STALL で諦めない。**
 * MPS を入れ替えた直後に 1 度だけ STALL を返すデバイスがある。
 * 復帰させて少し待ってから聞き直す。**回数は出す**ので、
 * 「何回目で通ったか」がログから分かる */
static int xhci_ep0_get_device_descriptor_retry(uint8_t slot_id, uint32_t tries) {
    uint32_t i;
    int rc = -1;
    for (i = 0; i < tries; i++) {
        rc = xhci_ep0_get_device_descriptor(slot_id);
        if (rc == 0) {
            if (i) {
                puts("[usb] device desc は ");
                putdec(i + 1);
                puts(" 回目で通った\r\n");
            }
            return 0;
        }
        (void)xhci_ep0_recover_stall(slot_id);
        usb_delay_ms(50);
    }
    return rc;
}

static int xhci_ep0_control_in(uint8_t slot_id, uint64_t setup, uint64_t data_phys, uint32_t len) {
    if (slot_id == 0 || g_ep0_ring_phys == 0 || data_phys == 0 || len == 0 || len > 4096) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)USB_VIRT((void*)g_ep0_ring_phys);
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cycle = g_ep0_cycle & 1U;

    ep[idx * 4 + 0] = (uint32_t)(setup & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(setup >> 32);
    ep[idx * 4 + 2] = 8;
    ep[idx * 4 + 3] = (2U << 10) | (1U << 6) | (3U << 16) | (1U << 4) | cycle; // Setup, IDT, TRT=IN, CHAIN

    idx++;
    ep[idx * 4 + 0] = (uint32_t)(data_phys & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(data_phys >> 32);
    ep[idx * 4 + 2] = len;
    ep[idx * 4 + 3] = (3U << 10) | (1U << 16) | (1U << 4) | cycle; // Data IN, CHAIN

    idx++;
    ep[idx * 4 + 0] = 0;
    ep[idx * 4 + 1] = 0;
    ep[idx * 4 + 2] = 0;
    ep[idx * 4 + 3] = (4U << 10) | (1U << 5) | cycle; // Status + IOC

    USB_MB(); /* TRB を書き終えてからドアベル */
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    xhci_ring_enqueue_advance(&g_ep0_enqueue_idx, &g_ep0_cycle, 3);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] ep0 IN cc=0x");
        puthex(cc);
        puts(" residual=0x");
        puthex(residual);
        puts(" len=0x");
        puthex(len);
        puts(" setup=0x");
        puthex(setup);
        puts("\r\n");
        return -3;
    }
    return 0;
}

static int xhci_ep0_control_no_data(uint8_t slot_id, uint64_t setup) {
    if (slot_id == 0 || g_ep0_ring_phys == 0) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)USB_VIRT((void*)g_ep0_ring_phys);
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cycle = g_ep0_cycle & 1U;

    ep[idx * 4 + 0] = (uint32_t)(setup & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(setup >> 32);
    ep[idx * 4 + 2] = 8;
    ep[idx * 4 + 3] = (2U << 10) | (1U << 6) | (1U << 4) | cycle; // Setup, IDT, TRT=No Data, CHAIN

    idx++;
    ep[idx * 4 + 0] = 0;
    ep[idx * 4 + 1] = 0;
    ep[idx * 4 + 2] = 0;
    ep[idx * 4 + 3] = (4U << 10) | (1U << 5) | (1U << 16) | cycle; // Status IN + IOC

    USB_MB(); /* TRB を書き終えてからドアベル */
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    xhci_ring_enqueue_advance(&g_ep0_enqueue_idx, &g_ep0_cycle, 2);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] ep0 no-data cc=0x");
        puthex(cc);
        puts(" residual=0x");
        puthex(residual);
        puts(" setup=0x");
        puthex(setup);
        puts("\r\n");
        return -3;
    }
    return 0;
}

static uint64_t usb_setup_packet(uint8_t bm_request_type, uint8_t b_request,
                                 uint16_t w_value, uint16_t w_index, uint16_t w_length) {
    uint64_t setup = 0;
    setup |= (uint64_t)bm_request_type;
    setup |= (uint64_t)b_request << 8;
    setup |= (uint64_t)w_value << 16;
    setup |= (uint64_t)w_index << 32;
    setup |= (uint64_t)w_length << 48;
    return setup;
}

/* ---- B-3: USB ハブ --------------------------------------------------------
 *
 * **Raspberry Pi 4 の 4 つの Type-A は、VL805 内蔵の USB2 ハブの向こうにある。**
 * xHCI のルートポートから見えるのはハブ 1 台だけで (実機で
 * vid=0x2109 pid=0x3431 class=0x09 を確認)、**ハブを列挙しない限り
 * キーボードには一生届かない。**
 *
 * 手順:
 *   1. ハブ記述子を読んでポート数を知る
 *   2. **スロット文脈に Hub ビットとポート数を入れる。**これを忘れると
 *      xHC はその先のデバイスを受け付けない
 *   3. 各ポートに電源を入れ、状態を見て、繋がっていればリセット
 *   4. 速度を確定して、Route String と TT を付けて Address Device
 *
 * ハブのクラス要求 (USB 2.0 仕様 11.24):
 *   ハブ記述子   bmRequestType 0xA0 / bRequest 6 / wValue 0x2900
 *   ポート状態   bmRequestType 0xA3 / bRequest 0 / wIndex=ポート / 4 バイト
 *   機能を立てる bmRequestType 0x23 / bRequest 3
 *   機能を落とす bmRequestType 0x23 / bRequest 1 */
#define HUB_FEAT_PORT_RESET   4U
#define HUB_FEAT_PORT_POWER   8U
#define HUB_FEAT_C_CONNECTION 16U
#define HUB_FEAT_C_RESET      20U

/* wPortStatus */
#define HUB_PORT_CONNECTION (1U << 0)
#define HUB_PORT_ENABLE     (1U << 1)
#define HUB_PORT_RESET      (1U << 4)
#define HUB_PORT_LOW_SPEED  (1U << 9)
#define HUB_PORT_HIGH_SPEED (1U << 10)

/* **記述子の先頭 8 バイトだけ読んで、EP0 の最大パケット長を確定させる。**
 * 8 バイトなら、相手の MPS が 8 でも 64 でも 1 パケットに収まるので必ず通る。
 * 失敗しても致命ではない (そのまま今の値で進む) */
__attribute__((unused))
static int xhci_ep0_probe_mps(uint8_t slot_id) {
    volatile uint8_t* d;
    uint32_t mps;
    if (slot_id == 0 || g_ep0_buf_phys == 0) return -1;
    if (g_usb_port_speed >= 3) return 0; /* 高速以上は速度で一意に決まる */

    memzero(USB_VIRT((void*)g_ep0_buf_phys), 16);
    if (xhci_ep0_control_in(slot_id, usb_setup_packet(0x80, 0x06, 0x0100, 0, 8),
                            g_ep0_buf_phys, 8) < 0) {
        (void)xhci_ep0_recover_stall(slot_id);
        return -2;
    }
    d = (volatile uint8_t*)USB_VIRT((void*)g_ep0_buf_phys);
    /* **読めた 8 バイトをそのまま出す。**bMaxPacketSize0 が本物かどうかは
     * これを見ないと言えない (bLength=18 / bDescriptorType=1 が揃っているか) */
    {
        uint32_t i;
        puts("[usb] dev desc[0..7]");
        for (i = 0; i < 8U; i++) { puts(" "); puthex_byte(d[i]); }
        puts("\r\n");
    }
    if (d[1] != 1) return -3;
    mps = d[7]; /* bMaxPacketSize0 */
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) return -4;

    puts("[usb] ep0 mps ");
    putdec(g_usb_ep0_mps);
    puts(" -> ");
    putdec(mps);
    puts("\r\n");
    if (mps == g_usb_ep0_mps) return 0;

    /* **xHC 側にも教えておく。**この後ポートを戻してやり直すので必須では
     * ないが、やり直しが失敗したときの保険になる。**失敗しても致命ではない** */
    (void)xhci_cmd_evaluate_context_mps(slot_id, mps);
    g_usb_ep0_mps = (uint16_t)mps;
    return 0;
}

static uint8_t g_hub_slot_id = 0;
static uint8_t g_hub_nbr_ports = 0;

/* ---- デバイスごとの文脈を退避する ------------------------------------------
 *
 * **EP0 のリングも入出力文脈もデバイスごとに違う。**
 * `xhci_cmd_address_device_full` は g_ep0_ring_phys などを**新しいデバイスの
 * ものに書き換える。**そのままハブに話しかけると、**子のリングにハブの
 * スロット番号で投げる**ことになり、当然通らない。
 *
 * 0 = ハブ / 1 = その先のデバイス */
#define USB_CTX_HUB 0
#define USB_CTX_DEV 1
static uint64_t g_sv_ep0_ring[2], g_sv_in_ctx[2], g_sv_out_ctx[2];
static uint32_t g_sv_ep0_idx[2], g_sv_ep0_cycle[2];
static uint16_t g_sv_mps[2];
static uint8_t  g_sv_speed[2];

/* ---- 出力デバイス文脈をそのまま出す ----------------------------------------
 *
 * **xHC が実際に持っている値**を見ないと、こちらが書いたつもりの値と
 * 突き合わせられない。入力文脈を出しても意味が無い (2026-08-18 codex 相談)。
 *
 * 出力文脈は index 0 = Slot Context / index 1 = EP0 Context。
 * 入力文脈と違って先頭に Input Control Context が無い */
static void xhci_dump_dev_ctx(const char* tag, uint64_t out_ctx_phys) {
    volatile uint32_t* oc;
    uint32_t stride_dw = g_xhci_ctx_size / 4U;
    uint32_t s0, s1, s2, s3, e0, e1, e4;

    if (out_ctx_phys == 0) {
        puts("[ctx] ");
        puts(tag);
        puts(" 文脈が無い\r\n");
        return;
    }
    oc = (volatile uint32_t*)USB_VIRT((void*)out_ctx_phys);
    s0 = oc[0]; s1 = oc[1]; s2 = oc[2]; s3 = oc[3];
    e0 = oc[stride_dw + 0]; e1 = oc[stride_dw + 1]; e4 = oc[stride_dw + 4];

    puts("[ctx] ");
    puts(tag);
    puts(" slot dw=");
    puthex(s0); puts(" "); puthex(s1); puts(" "); puthex(s2); puts(" "); puthex(s3);
    puts("\r\n[ctx]   route="); puthex(s0 & 0xFFFFFU);
    puts(" speed="); putdec((s0 >> 20) & 0xFU);
    puts(" mtt="); putdec((s0 >> 25) & 1U);
    puts(" hub="); putdec((s0 >> 26) & 1U);
    puts(" ctxent="); putdec((s0 >> 27) & 0x1FU);
    puts("\r\n[ctx]   rhport="); putdec((s1 >> 16) & 0xFFU);
    puts(" nports="); putdec((s1 >> 24) & 0xFFU);
    puts(" ttslot="); putdec(s2 & 0xFFU);
    puts(" ttport="); putdec((s2 >> 8) & 0xFFU);
    puts(" ttt="); putdec((s2 >> 16) & 3U);
    puts("\r\n[ctx]   addr="); putdec(s3 & 0xFFU);
    /* Slot State: 0=Disabled/Enabled 1=Default 2=Addressed 3=Configured */
    puts(" slotstate="); putdec((s3 >> 27) & 0x1FU);
    puts("\r\n[ctx] ");
    puts(tag);
    puts(" ep0 dw=");
    puthex(e0); puts(" "); puthex(e1); puts(" "); puthex(e4);
    /* EP State: 0=Disabled 1=Running 2=Halted 3=Stopped 4=Error */
    puts("\r\n[ctx]   epstate="); putdec(e0 & 7U);
    puts(" cerr="); putdec((e1 >> 1) & 3U);
    puts(" eptype="); putdec((e1 >> 3) & 7U);
    puts(" mps="); putdec((e1 >> 16) & 0xFFFFU);
    puts(" avgtrb="); putdec(e4 & 0xFFFFU);
    puts("\r\n");
}

static void usb_ctx_store(int i) {
    g_sv_ep0_ring[i]  = g_ep0_ring_phys;
    g_sv_in_ctx[i]    = g_input_ctx_phys;
    g_sv_out_ctx[i]   = g_output_ctx_phys;
    g_sv_ep0_idx[i]   = g_ep0_enqueue_idx;
    g_sv_ep0_cycle[i] = g_ep0_cycle;
    g_sv_mps[i]       = g_usb_ep0_mps;
    g_sv_speed[i]     = g_usb_port_speed;
}

__attribute__((unused))
static void usb_ctx_restore(int i) {
    g_ep0_ring_phys   = g_sv_ep0_ring[i];
    g_input_ctx_phys  = g_sv_in_ctx[i];
    g_output_ctx_phys = g_sv_out_ctx[i];
    g_ep0_enqueue_idx = g_sv_ep0_idx[i];
    g_ep0_cycle       = g_sv_ep0_cycle[i];
    g_usb_ep0_mps     = g_sv_mps[i];
    g_usb_port_speed  = g_sv_speed[i];
}

/* ---- 探針: 1 回の転送で見るものを全部出す ---------------------------------
 *
 * **投入前**にリングの物理番地・enqueue 位置・cycle を出し、**失敗したら**
 * イベントが指す TRB からどの段 (Setup / Data / Status) で落ちたかを出す。
 * これが無いと「STALL した」以上のことが言えない (2026-08-18 codex 相談) */
static int usb_probe_desc(const char* tag, uint8_t slot_id, uint32_t len) {
    uint64_t ring = g_ep0_ring_phys;
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cyc = g_ep0_cycle & 1U;
    int rc;

    puts("[pb] ");
    puts(tag);
    puts(" ring=");
    puthex(ring);
    puts(" idx=");
    putdec(idx);
    puts(" cycle=");
    putdec(cyc);
    puts(" mps=");
    putdec(g_usb_ep0_mps);
    puts(" len=");
    putdec(len);
    puts("\r\n");

    g_last_evt_trb = 0;
    memzero(USB_VIRT((void*)g_ep0_buf_phys), 32);
    rc = xhci_ep0_control_in(slot_id, usb_setup_packet(0x80, 0x06, 0x0100, 0, (uint16_t)len),
                             g_ep0_buf_phys, len);

    puts("[pb] ");
    puts(tag);
    if (rc == 0) {
        volatile uint8_t* d = (volatile uint8_t*)USB_VIRT((void*)g_ep0_buf_phys);
        uint32_t k, n = (len < 8U) ? len : 8U;
        puts(" ok");
        for (k = 0; k < n; k++) { puts(" "); puthex_byte(d[k]); }
        puts("\r\n");
        return 0;
    }

    puts(" *** 落ちた rc=");
    puthex((uint64_t)(uint32_t)(-rc));
    puts(" evt_trb=");
    puthex(g_last_evt_trb);
    puts(" 段=");
    if (g_last_evt_trb == ring + (uint64_t)idx * 16U)             puts("Setup");
    else if (g_last_evt_trb == ring + (uint64_t)(idx + 1U) * 16U) puts("Data");
    else if (g_last_evt_trb == ring + (uint64_t)(idx + 2U) * 16U) puts("Status");
    else                                                          puts("不明");
    puts("\r\n");
    /* **落ちた直後の EP0 が Halted (EP State=2) かを見る。**
     * Halted なら xHC は本当に相手からの STALL を受けている */
    xhci_dump_dev_ctx(tag, g_output_ctx_phys);
    return rc;
}

/* ---- EP0 が STALL したときの復帰 -------------------------------------------
 *
 * **STALL (cc=6) を受けると EP0 は Halted になり、以後の転送は完了イベントを
 * 返さなくなる。**1 回の STALL が、そのデバイスとのやり取り全部を殺す。
 * 実機で実際にそうなった (SET_FEATURE が STALL したあと、記述子の読み出しが
 * 全部 rc=-2 = イベント来ずになった)。
 *
 * 戻すには **Reset Endpoint** で Halted を落とし、**Set TR Dequeue Pointer**
 * で xHC 側の取り出し位置をこちらと合わせ直す。片方だけでは戻らない */
static int xhci_ep0_recover_stall(uint8_t slot_id) {
    uint64_t cmd_ptr;
    uint8_t cc = 0xFF, slot = 0;
    if (slot_id == 0 || g_ep0_ring_phys == 0) return -1;

    cmd_ptr = xhci_cmd_submit(0, 0, 0,
                              (14U << 10) | (1U << 16) | ((uint32_t)slot_id << 24));
    if (!cmd_ptr) return -2;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) return -3;
    if (cc != 1) return -4;

    xhci_ring_reset(g_ep0_ring_phys);
    g_ep0_enqueue_idx = 0;
    g_ep0_cycle = 1;
    USB_MB();

    cc = 0xFF;
    cmd_ptr = xhci_cmd_submit((uint32_t)((g_ep0_ring_phys & ~0xFULL) | 1ULL),
                              (uint32_t)(g_ep0_ring_phys >> 32), 0,
                              (16U << 10) | (1U << 16) | ((uint32_t)slot_id << 24));
    if (!cmd_ptr) return -5;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) return -6;
    return (cc == 1) ? 0 : -7;
}

static int usb_hub_get_descriptor(uint8_t slot_id, uint8_t* out_nbr_ports, uint8_t* out_mtt) {
    volatile uint8_t* buf;
    uint64_t setup = usb_setup_packet(0xA0, 0x06, 0x2900, 0, 9);
    if (!g_ep0_cfg_buf_phys) {
        void* b = usb_alloc_dma(1);
        if (!b) return -1;
        g_ep0_cfg_buf_phys = (uint64_t)b;
    }
    buf = (volatile uint8_t*)USB_VIRT((void*)g_ep0_cfg_buf_phys);
    memzero((void*)buf, 64);
    if (xhci_ep0_control_in(slot_id, setup, g_ep0_cfg_buf_phys, 9) < 0) return -2;
    if (buf[1] != 0x29) return -3;               /* bDescriptorType = HUB */
    if (out_nbr_ports) *out_nbr_ports = buf[2];  /* bNbrPorts */
    /* wHubCharacteristics の bit 7 (TT Think Time の上) ではなく、
     * **複数 TT かどうかは interface protocol で決まる。**ここでは単一 TT
     * として扱う (実機の VL805 内蔵ハブも QEMU の usb-hub も単一) */
    if (out_mtt) *out_mtt = 0;
    return 0;
}

/* **ポートへの要求はデバイスが Configured になってからでないと通らない。**
 *
 * USB 2.0 仕様 9.4: Address 状態で受け付けられるのはデバイス宛の標準要求まで。
 * ハブのポート要求は recipient=Other なので、**設定を選ぶ前に投げると STALL する。**
 * 実機の VL805 内蔵ハブがまさにそれで、SET_FEATURE(PORT_POWER) が cc=6 を返し、
 * その後 EP0 が Halted になって以降の転送が全部死んだ (2026-08-17 実機)。
 *
 * QEMU の usb-hub は緩くて通ってしまうので、**ここは実機でしか出ない** */
static int usb_hub_configure(uint8_t slot_id) {
    volatile uint8_t* buf;
    uint8_t cfg_value;
    if (!g_ep0_cfg_buf_phys) {
        void* b = usb_alloc_dma(1);
        if (!b) return -1;
        g_ep0_cfg_buf_phys = (uint64_t)b;
    }
    buf = (volatile uint8_t*)USB_VIRT((void*)g_ep0_cfg_buf_phys);
    memzero((void*)buf, 16);
    if (xhci_ep0_control_in(slot_id, usb_setup_packet(0x80, 0x06, 0x0200, 0, 9),
                            g_ep0_cfg_buf_phys, 9) < 0) return -2;
    if (buf[1] != 2) return -3;                       /* bDescriptorType = CONFIGURATION */
    cfg_value = buf[5];                               /* bConfigurationValue */
    if (cfg_value == 0) cfg_value = 1;
    return xhci_ep0_control_no_data(slot_id, usb_setup_packet(0x00, 0x09, cfg_value, 0, 0));
}

static int usb_hub_port_feature(uint8_t slot_id, uint8_t port, uint16_t feat, int set) {
    uint64_t setup = usb_setup_packet(0x23, set ? 0x03 : 0x01, feat, port, 0);
    int rc = xhci_ep0_control_no_data(slot_id, setup);
    /* **STALL したら必ず戻す。**放っておくと以後の転送が全部無応答になる */
    if (rc < 0) (void)xhci_ep0_recover_stall(slot_id);
    return rc;
}

static int usb_hub_get_port_status(uint8_t slot_id, uint8_t port, uint32_t* out_status) {
    volatile uint8_t* buf;
    uint64_t setup = usb_setup_packet(0xA3, 0x00, 0, port, 4);
    if (!g_ep0_cfg_buf_phys) return -1;
    buf = (volatile uint8_t*)USB_VIRT((void*)g_ep0_cfg_buf_phys);
    memzero((void*)buf, 8);
    if (xhci_ep0_control_in(slot_id, setup, g_ep0_cfg_buf_phys, 4) < 0) {
        (void)xhci_ep0_recover_stall(slot_id);
        return -2;
    }
    if (out_status) {
        *out_status = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                      ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    }
    return 0;
}

/* **スロット文脈に Hub ビットとポート数を入れる。**
 * Configure Endpoint (A0 のみ) で入る。Evaluate Context では入らない —
 * あちらが触れるのは Max Exit Latency / Interrupter Target と、
 * **EP0 の Max Packet Size** (A1) */
static int usb_hub_mark_slot(uint8_t slot_id, uint8_t nbr_ports, uint8_t mtt) {
    volatile uint32_t* ic;
    uint32_t slot_off = (g_xhci_ctx_size / 4U) * 1U;
    if (!g_input_ctx_phys) return -1;
    ic = (volatile uint32_t*)USB_VIRT((void*)g_input_ctx_phys);
    ic[slot_off + 0] |= (1U << 26) | (mtt ? (1U << 25) : 0U);   /* Hub / MTT */
    ic[slot_off + 1] = (ic[slot_off + 1] & 0x00FFFFFFU) | ((uint32_t)nbr_ports << 24);
    return xhci_cmd_configure_endpoint(slot_id);
}

/* **ハブのポートを 1 本リセットして、有効になるまで待つ。**
 * 相手を初期状態に戻したいときにも使う (制御エンドポイントが詰まったとき)。
 * 戻り値: 0 = 有効になった / -1 = ならなかった。*out_status に最後の状態 */
static int usb_hub_reset_port(uint8_t slot_id, uint8_t port, uint32_t* out_status) {
    uint32_t st = 0;
    uint32_t i;

    if (usb_hub_port_feature(slot_id, port, HUB_FEAT_PORT_RESET, 1) < 0) return -1;
    for (i = 0; i < 100U; i++) {
        usb_delay_ms(10);
        if (usb_hub_get_port_status(slot_id, port, &st) < 0) break;
        if (!(st & HUB_PORT_RESET) && (st & HUB_PORT_ENABLE)) break;
    }
    (void)usb_hub_port_feature(slot_id, port, HUB_FEAT_C_RESET, 0);
    /* **仕様が要求する回復時間 (TRSTRCY = 10ms)。**
     * これを待たずにアドレスを振ると、相手がまだ受け付けない */
    usb_delay_ms(20);
    if (out_status) *out_status = st;
    return (st & HUB_PORT_ENABLE) ? 0 : -1;
}

/* ハブの各ポートを見て、**最初に繋がっているデバイス**のポート番号と速度を返す。
 * 戻り値: 0 = 見つかった / -1 = 何も繋がっていない */
static int usb_hub_find_device(uint8_t slot_id, uint8_t nbr_ports,
                               uint8_t* out_port, uint32_t* out_speed) {
    uint8_t p;
    for (p = 1; p <= nbr_ports; p++) {
        uint32_t st = 0;

        /* 電源を入れる。**入れないと接続が見えないハブがある。**
         * bPwrOn2PwrGood は最大 255 * 2ms だが、内蔵ハブは既に入っているので
         * 100ms 見ておけば足りる */
        (void)usb_hub_port_feature(slot_id, p, HUB_FEAT_PORT_POWER, 1);
        usb_delay_ms(100);

        if (usb_hub_get_port_status(slot_id, p, &st) < 0) continue;
        puts("[usb] hub port ");
        putdec(p);
        puts(" status=");
        puthex(st);
        puts((st & HUB_PORT_CONNECTION) ? "  接続あり\r\n" : "\r\n");
        if (!(st & HUB_PORT_CONNECTION)) continue;

        (void)usb_hub_port_feature(slot_id, p, HUB_FEAT_C_CONNECTION, 0);

        /* リセットして有効にする。**ルートポートと同じで、接続だけでは
         * Address Device は通らない** */
        (void)usb_hub_reset_port(slot_id, p, &st);

        puts("[usb] hub port ");
        putdec(p);
        puts(" reset -> status=");
        puthex(st);
        puts((st & HUB_PORT_ENABLE) ? "  有効になった\r\n" : "  *** 有効にならなかった\r\n");
        if (!(st & HUB_PORT_ENABLE)) continue;

        if (out_port) *out_port = p;
        /* 速度は**ハブのポート状態からしか分からない。**ルートの PORTSC を
         * 見てもハブ自身の速度が返るだけ */
        if (out_speed) {
            *out_speed = (st & HUB_PORT_LOW_SPEED)  ? 2U :
                         (st & HUB_PORT_HIGH_SPEED) ? 3U : 1U;
        }
        return 0;
    }
    return -1;
}

static int xhci_ep0_get_config_descriptor(uint8_t slot_id) {
    if (slot_id == 0) return -1;
    if (!g_ep0_cfg_buf_phys) {
        void* b = usb_alloc_dma(1);
        if (!b) return -2;
        memzero(USB_VIRT(b), PAGE_SIZE);
        g_ep0_cfg_buf_phys = (uint64_t)b;
    }

    volatile uint8_t* cfg = (volatile uint8_t*)USB_VIRT((void*)g_ep0_cfg_buf_phys);
    memzero((void*)cfg, PAGE_SIZE);

    // 1) First read config header (9 bytes) to get wTotalLength.
    uint64_t setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0200 << 16;
    setup |= (uint64_t)9 << 48;
    {
        int rc = xhci_ep0_control_in(slot_id, setup, g_ep0_cfg_buf_phys, 9);
        if (rc < 0) {
            puts("[usb] GET_DESCRIPTOR(Config hdr) rc=0x");
            puthex((uint64_t)(uint32_t)(-rc));
            puts("\r\n");
            return -3;
        }
    }
    if (cfg[1] != 2) return -4;
    uint16_t total_len = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
    if (total_len < 9) return -5;
    if (total_len > 255) total_len = 255;

    // 2) Read full configuration descriptor set.
    setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0200 << 16;
    setup |= (uint64_t)total_len << 48;
    {
        int rc = xhci_ep0_control_in(slot_id, setup, g_ep0_cfg_buf_phys, total_len);
        if (rc < 0) {
            puts("[usb] GET_DESCRIPTOR(Config body) rc=0x");
            puthex((uint64_t)(uint32_t)(-rc));
            puts(" total=0x");
            puthex(total_len);
            puts("\r\n");
            return -6;
        }
    }
    if (cfg[1] != 2) return -7;

    g_usb_cfg_desc_ready = 1;
    g_usb_msc_if_ready = 0;
    g_usb_cfg_value = cfg[5];
    g_usb_msc_if_class = 0;
    g_usb_msc_if_subclass = 0;
    g_usb_msc_if_proto = 0;
    g_usb_msc_if_number = 0;
    g_usb_bulk_out_ep = 0;
    g_usb_bulk_in_ep = 0;
    g_usb_bulk_out_mps = 0;
    g_usb_bulk_in_mps = 0;
    uint8_t current_if_num = 0xFF;
    int current_if_is_msc = 0;
    int current_if_is_hid = 0;
    g_usb_hid_if_ready = 0;
    g_usb_int_in_ep = 0;
    g_usb_int_in_mps = 0;
    g_usb_int_in_interval = 0;

    uint16_t off = 0;
    while (off + 2 <= total_len) {
        uint8_t len = cfg[off + 0];
        uint8_t typ = cfg[off + 1];
        if (len < 2) break;
        if (off + len > total_len) break;
        if (typ == 4 && len >= 9) {
            current_if_num = cfg[off + 2];
            uint8_t c = cfg[off + 5];
            uint8_t s = cfg[off + 6];
            uint8_t p = cfg[off + 7];
            current_if_is_msc = 0;
            current_if_is_hid = 0;
            /* **HID。** サブクラスが 1 なら boot protocol に対応している。
             * プロトコル 1 = キーボード。**サブクラスが 0 のものは
             * レポートデスクリプタを読まないと使えない**ので採らない */
            if (c == 0x03 && s == 0x01 && p == 0x01 && !g_usb_hid_if_ready) {
                g_usb_hid_if_ready = 1;
                g_usb_hid_if_number = current_if_num;
                g_usb_hid_if_proto = p;
                current_if_is_hid = 1;
            }
            if (c == 0x08 && s == 0x06 && p == 0x50) {
                g_usb_msc_if_ready = 1;
                g_usb_msc_if_class = c;
                g_usb_msc_if_subclass = s;
                g_usb_msc_if_proto = p;
                g_usb_msc_if_number = current_if_num;
                current_if_is_msc = 1;
            }
        } else if (typ == 5 && len >= 7 && current_if_is_hid) {
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3] & 0x03U;
            /* **割り込み (3) の IN (bit7) だけ。** キーボードは OUT を
             * 持つこともある (LED 用) が、いまは使わない */
            if (attr == 3 && (ep_addr & 0x80U) && g_usb_int_in_ep == 0) {
                g_usb_int_in_ep = ep_addr;
                g_usb_int_in_mps = (uint16_t)cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
                g_usb_int_in_interval = cfg[off + 6];
            }
        } else if (typ == 5 && len >= 7 && current_if_is_msc) {
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3] & 0x03U;
            uint16_t mps = (uint16_t)cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
            if (attr == 2) { // bulk
                if (ep_addr & 0x80U) {
                    g_usb_bulk_in_ep = ep_addr;
                    g_usb_bulk_in_mps = mps;
                } else {
                    g_usb_bulk_out_ep = ep_addr;
                    g_usb_bulk_out_mps = mps;
                }
            }
        }
        off += len;
    }
    /* **どちらか見つかれば成功。** 以前は大容量記憶装置が無いだけで -8 を
     * 返していたので、キーボードを挿すと「Config デスクリプタが読めない」
     * ように見えていた。**読めている。探し物が無かっただけ** */
    if (!g_usb_msc_if_ready && !g_usb_hid_if_ready) return -8;
    if (g_usb_hid_if_ready) {
        if (g_usb_int_in_ep == 0) return -10;
        g_usb_int_in_dci = xhci_dci_from_epaddr(g_usb_int_in_ep);
    }
    if (g_usb_msc_if_ready) {
        if (g_usb_bulk_out_ep == 0 || g_usb_bulk_in_ep == 0) return -9;
        g_usb_bulk_out_dci = xhci_dci_from_epaddr(g_usb_bulk_out_ep);
        g_usb_bulk_in_dci = xhci_dci_from_epaddr(g_usb_bulk_in_ep);
    }
    return 0;
}

static int xhci_setup_bulk_endpoints(uint8_t slot_id) {
    if (slot_id == 0 || g_input_ctx_phys == 0 || g_output_ctx_phys == 0) return -1;
    if (g_usb_bulk_out_dci == 0 || g_usb_bulk_in_dci == 0) return -2;

    if (!g_bulk_out_ring_phys) {
        void* p = usb_alloc_dma(1);
        if (!p) return -3;
        g_bulk_out_ring_phys = (uint64_t)p;
    }
    if (!g_bulk_in_ring_phys) {
        void* p = usb_alloc_dma(1);
        if (!p) return -4;
        g_bulk_in_ring_phys = (uint64_t)p;
    }
    if (!g_bulk_buf_phys) {
        void* p = usb_alloc_dma(USB_BULK_BUF_PAGES);
        if (!p) return -5;
        g_bulk_buf_phys = (uint64_t)p;
    }
    xhci_ring_reset(g_bulk_out_ring_phys);
    xhci_ring_reset(g_bulk_in_ring_phys);
    g_bulk_out_enqueue_idx = 0;
    g_bulk_out_cycle = 1;
    g_bulk_in_enqueue_idx = 0;
    g_bulk_in_cycle = 1;
    memzero(USB_VIRT((void*)g_bulk_buf_phys), USB_BULK_BUF_SIZE);

    volatile uint32_t* ic = (volatile uint32_t*)USB_VIRT((void*)g_input_ctx_phys);
    memzero((void*)ic, PAGE_SIZE);
    // Input context begins with Input Control Context. Copy the current device
    // context payload after it so controller sees current slot/EP0 state plus our adds.
    {
        uint32_t stride_dw = g_xhci_ctx_size / 4U;
        volatile uint32_t* oc = (volatile uint32_t*)USB_VIRT((void*)g_output_ctx_phys);
        for (uint32_t dci = 0; dci <= 31; dci++) {
            uint32_t in_off = stride_dw * (1U + dci);
            uint32_t out_off = stride_dw * dci;
            for (uint32_t j = 0; j < stride_dw; j++) {
                ic[in_off + j] = oc[out_off + j];
            }
        }
    }
    ic[0] = 0;
    // For Configure Endpoint, A0 identifies the slot context; A1 must remain 0.
    ic[1] = (1U << 0) | (1U << g_usb_bulk_out_dci) | (1U << g_usb_bulk_in_dci);

    uint8_t max_dci = (g_usb_bulk_out_dci > g_usb_bulk_in_dci) ? g_usb_bulk_out_dci : g_usb_bulk_in_dci;
    uint32_t slot_off = xhci_ctx_offset_dw(0);
    ic[slot_off + 0] = (ic[slot_off + 0] & ~(0x1FU << 27)) | ((uint32_t)max_dci << 27);

    {
        uint32_t off = xhci_ctx_offset_dw(1);
        ic[off + 0] = 0;
        ic[off + 1] = (4U << 3) | ((uint32_t)g_usb_ep0_mps << 16);
        ic[off + 2] = (uint32_t)((g_ep0_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_ep0_ring_phys >> 32);
        ic[off + 4] = 8;
    }

    {
        uint32_t off = xhci_ctx_offset_dw(g_usb_bulk_out_dci);
        ic[off + 0] = 0;
        ic[off + 1] = (2U << 3) | (3U << 1) | ((uint32_t)g_usb_bulk_out_mps << 16);
        ic[off + 2] = (uint32_t)((g_bulk_out_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_bulk_out_ring_phys >> 32);
        ic[off + 4] = g_usb_bulk_out_mps;
    }

    {
        uint32_t off = xhci_ctx_offset_dw(g_usb_bulk_in_dci);
        ic[off + 0] = 0;
        ic[off + 1] = (6U << 3) | (3U << 1) | ((uint32_t)g_usb_bulk_in_mps << 16);
        ic[off + 2] = (uint32_t)((g_bulk_in_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_bulk_in_ring_phys >> 32);
        ic[off + 4] = g_usb_bulk_in_mps;
    }

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (12U << 10) | ((uint32_t)slot_id << 24)
    );
    if (!cmd_ptr) return -6;
    uint8_t cc = 0xFF;
    uint8_t slot = 0;
    int w = xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot);
    if (w < 0) return -7;
    (void)slot;
    if (cc != 1) {
        puts("[usb] Configure Endpoint(BULK) cc=0x");
        puthex(cc);
        puts("\r\n");
    }
    return (cc == 1) ? 0 : -8;
}

static int xhci_bulk_transfer_ex(uint8_t slot_id, uint8_t dci, uint64_t ring_phys, uint64_t data_phys,
                                 uint32_t len, int in_dir, int log_errors) {
    (void)log_errors;   /* いまは常に出す。呼び分けは未使用 */
    if (slot_id == 0 || dci == 0 || ring_phys == 0 || data_phys == 0) return -1;

    volatile uint32_t* ring = (volatile uint32_t*)USB_VIRT((void*)ring_phys);
    uint32_t* idx = in_dir ? &g_bulk_in_enqueue_idx : &g_bulk_out_enqueue_idx;
    uint32_t* cycle = in_dir ? &g_bulk_in_cycle : &g_bulk_out_cycle;
    uint32_t trb = *idx;
    uint32_t pcs = *cycle & 1U;
    uint64_t trb_phys = ring_phys + (uint64_t)trb * 16ULL;
    ring[trb * 4 + 0] = (uint32_t)(data_phys & 0xFFFFFFFFU);
    ring[trb * 4 + 1] = (uint32_t)(data_phys >> 32);
    ring[trb * 4 + 2] = len;
    ring[trb * 4 + 3] = (1U << 10) | (1U << 5) | pcs; // Normal TRB + IOC + cycle

    USB_MB(); /* TRB を書き終えてからドアベル */
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, dci);
    xhci_ring_enqueue_advance(idx, cycle, 1);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, dci, trb_phys, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] bulk ");
        puts(in_dir ? "IN" : "OUT");
        puts(" dci=0x");
        puthex(dci);
        puts(" cc=0x");
        puthex(cc);
        puts(" residual=0x");
        puthex(residual);
        puts(" len=0x");
        puthex(len);
        puts("\r\n");
    }
    return (cc == 1) ? 0 : -3;
}

/* ---- HID キーボード -------------------------------------------------------
 *
 * **bulk と同じ道具立てで済む。** 違うのは 2 つだけ:
 *   - エンドポイントの種別が 7 (Interrupt In)。bulk IN は 6
 *   - Interval (ポーリング間隔) を EP コンテキストに入れる
 *
 * TRB の積み方 (Normal TRB + IOC) もイベントの待ち方も bulk と同一。 */
static int xhci_setup_interrupt_endpoint(uint8_t slot_id) {
    if (slot_id == 0 || g_input_ctx_phys == 0 || g_output_ctx_phys == 0) return -1;
    if (g_usb_int_in_dci == 0) return -2;

    if (!g_int_ring_phys) {
        void* p = usb_alloc_dma(1);
        if (!p) return -3;
        g_int_ring_phys = (uint64_t)p;
    }
    if (!g_int_buf_phys) {
        void* p = usb_alloc_dma(1);
        if (!p) return -4;
        g_int_buf_phys = (uint64_t)p;
    }
    xhci_ring_reset(g_int_ring_phys);
    g_int_enqueue_idx = 0;
    g_int_cycle = 1;
    memzero(USB_VIRT((void*)g_int_buf_phys), PAGE_SIZE);

    volatile uint32_t* ic = (volatile uint32_t*)USB_VIRT((void*)g_input_ctx_phys);
    memzero((void*)ic, PAGE_SIZE);
    {
        uint32_t stride_dw = g_xhci_ctx_size / 4U;
        volatile uint32_t* oc = (volatile uint32_t*)USB_VIRT((void*)g_output_ctx_phys);
        for (uint32_t dci = 0; dci <= 31; dci++) {
            uint32_t in_off = stride_dw * (1U + dci);
            uint32_t out_off = stride_dw * dci;
            for (uint32_t j = 0; j < stride_dw; j++) ic[in_off + j] = oc[out_off + j];
        }
    }
    ic[0] = 0;
    ic[1] = (1U << 0) | (1U << g_usb_int_in_dci);

    uint32_t slot_off = xhci_ctx_offset_dw(0);
    ic[slot_off + 0] = (ic[slot_off + 0] & ~(0x1FU << 27)) |
                       ((uint32_t)g_usb_int_in_dci << 27);

    {
        uint32_t off = xhci_ctx_offset_dw(1);
        ic[off + 0] = 0;
        ic[off + 1] = (4U << 3) | ((uint32_t)g_usb_ep0_mps << 16);
        ic[off + 2] = (uint32_t)((g_ep0_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_ep0_ring_phys >> 32);
        ic[off + 4] = 8;
    }

    {
        uint32_t off = xhci_ctx_offset_dw(g_usb_int_in_dci);
        /* **Interval は DW0 の [23:16]。** 125us を 1 として 2 のべき乗で
         * 数える。デスクリプタの bInterval (ミリ秒) をそのまま入れては
         * いけないが、**遅いぶんには取りこぼすだけで壊れない**ので、
         * 安全側の 7 (= 2^7 x 125us = 16ms) に寄せる */
        uint32_t interval = 7;
        ic[off + 0] = (interval << 16);
        ic[off + 1] = (7U << 3) | (3U << 1) | ((uint32_t)g_usb_int_in_mps << 16);
        ic[off + 2] = (uint32_t)((g_int_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_int_ring_phys >> 32);
        ic[off + 4] = g_usb_int_in_mps;
    }

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (12U << 10) | ((uint32_t)slot_id << 24));
    if (!cmd_ptr) return -5;
    uint8_t cc = 0xFF, slot = 0;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) return -6;
    (void)slot;
    if (cc != 1) {
        puts("[usb] Configure Endpoint(INT) cc=0x");
        puthex(cc);
        puts("\r\n");
        return -7;
    }
    return 0;
}

/* SET_CONFIGURATION -> SET_PROTOCOL(boot) -> SET_IDLE。
 *
 * **boot protocol にするのが要点。** そうしないとレポートの形が
 * HID レポートデスクリプタ次第になり、解釈器が要る。
 * SET_IDLE(0) は「変化があったときだけ報告しろ」— 押しっぱなしで
 * 同じレポートが繰り返し来るのを止める */
int usb_hid_keyboard_init(void) {
    if (g_xhci_slot_id == 0 || !g_usb_hid_if_ready) return -1;

    if (xhci_ep0_control_no_data(g_xhci_slot_id,
            usb_setup_packet(0x00, 9, g_usb_cfg_value, 0, 0)) < 0) return -2;

    if (xhci_setup_interrupt_endpoint(g_xhci_slot_id) < 0) return -3;

    /* **失敗しても続ける。** 対応していないキーボードがある。
     * boot protocol が既定のものも多い */
    (void)xhci_ep0_control_no_data(g_xhci_slot_id,
            usb_setup_packet(0x21, 0x0B, 0, g_usb_hid_if_number, 0));   /* SET_PROTOCOL(boot) */
    (void)xhci_ep0_control_no_data(g_xhci_slot_id,
            usb_setup_packet(0x21, 0x0A, 0, g_usb_hid_if_number, 0));   /* SET_IDLE(0) */

    g_usb_kbd_ready = 1;
    return 0;
}

int usb_hid_keyboard_ready(void) { return g_usb_kbd_ready; }

/* 8 バイトのレポートを 1 つ取る。**待たない。**
 * 0 = 取れた / 1 = まだ来ていない / 負 = 故障
 *
 * ---- 積みっぱなしにする ----------------------------------------------------
 *
 * **呼ばれるたびに TRB を積んではいけない。** 割り込みエンドポイントは
 * 「キーが押されるまで完了しない」ので、押されない間に積み続けると
 * リングが埋まり、いざ押されたとき完了するのは**最初に積んだ TRB**。
 * こちらが「今積んだ TRB」の完了を探していると永久に一致しない
 * (実測: sendkey を 6 回送って受け取り 0)。
 *
 * **1 つ積んだら、完了するまで積み直さない。**照合も TRB の番地では行わず、
 * スロットとエンドポイントだけで見る */
static int g_int_outstanding = 0;

int usb_hid_keyboard_poll(uint8_t report[8]) {
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (!g_usb_kbd_ready || !report) return -1;

    if (!g_int_outstanding) {
        volatile uint32_t* ring = (volatile uint32_t*)USB_VIRT((void*)g_int_ring_phys);
        uint32_t trb = g_int_enqueue_idx;
        uint32_t pcs = g_int_cycle & 1U;
        memzero(USB_VIRT((void*)g_int_buf_phys), 8);
        ring[trb * 4 + 0] = (uint32_t)(g_int_buf_phys & 0xFFFFFFFFU);
        ring[trb * 4 + 1] = (uint32_t)(g_int_buf_phys >> 32);
        ring[trb * 4 + 2] = 8;
        ring[trb * 4 + 3] = (1U << 10) | (1U << 5) | pcs;   /* Normal TRB + IOC */
        USB_MB(); /* TRB を書き終えてからドアベル */
        mmio_write32(g_db_regs, (uint32_t)g_xhci_slot_id * 4U, g_usb_int_in_dci);
        xhci_ring_enqueue_advance(&g_int_enqueue_idx, &g_int_cycle, 1);
        g_int_outstanding = 1;
    }

    /* **待ちは短く。** キーが来ていないのが普通の状態で、ここで長く回すと
     * 呼び出し側 (DOOM の描画) が止まる */
    if (xhci_poll_transfer_event(g_xhci_slot_id, g_usb_int_in_dci, 0,
                                 2000, &cc, &residual) < 0) {
        return 1;   /* まだ来ていない。TRB は積んだまま */
    }
    g_int_outstanding = 0;
    if (cc != 1 && cc != 13) return -2;   /* 13 = Short Packet。8 未満でも可 */

    {
        const volatile uint8_t* b = (const volatile uint8_t*)USB_VIRT((void*)g_int_buf_phys);
        for (int i = 0; i < 8; i++) report[i] = b[i];
    }
    return 0;
}

static int usb_msc_reset_recovery(void) {
    if (g_xhci_slot_id == 0 || !g_usb_msc_if_ready) return -1;
    if (xhci_ep0_control_no_data(g_xhci_slot_id,
                                 usb_setup_packet(0x21, 0xFF, 0, g_usb_msc_if_number, 0)) < 0) {
        return -2;
    }
    if (g_usb_bulk_out_ep) {
        if (xhci_ep0_control_no_data(g_xhci_slot_id,
                                     usb_setup_packet(0x02, 0x01, 0, g_usb_bulk_out_ep, 0)) < 0) {
            return -3;
        }
    }
    if (g_usb_bulk_in_ep) {
        if (xhci_ep0_control_no_data(g_xhci_slot_id,
                                     usb_setup_packet(0x02, 0x01, 0, g_usb_bulk_in_ep, 0)) < 0) {
            return -4;
        }
    }
    return 0;
}

static int usb_msc_recover_bulk_path(void) {
    if (g_xhci_slot_id == 0) return -1;
    if (g_usb_bulk_out_dci == 0 || g_usb_bulk_in_dci == 0) return -2;
    (void)usb_msc_reset_recovery();
    return xhci_setup_bulk_endpoints(g_xhci_slot_id);
}

static int usb_msc_refresh_bulk_rings_if_needed(void) {
    if (g_xhci_slot_id == 0) return -1;
    return xhci_setup_bulk_endpoints(g_xhci_slot_id);
}

static int usb_msc_bot_command(const uint8_t* cdb, uint8_t cdb_len, void* data, uint32_t data_len, int data_in) {
    if (!g_usb_msc_if_ready || g_xhci_slot_id == 0) return -1;
    if (!cdb || cdb_len == 0 || cdb_len > 16) return -2;
    if (data_len > USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET) return -3;
    if (!g_bulk_buf_phys || !g_bulk_out_ring_phys || !g_bulk_in_ring_phys) return -4;
    if (g_usb_bulk_out_dci == 0 || g_usb_bulk_in_dci == 0) return -5;
    if (usb_msc_refresh_bulk_rings_if_needed() < 0) return -6;

    for (int attempt = 0; attempt < 2; attempt++) {
        volatile uint8_t* buf = (volatile uint8_t*)USB_VIRT((void*)g_bulk_buf_phys);
        memzero((void*)buf, USB_BULK_BUF_SIZE);

        struct usb_msc_cbw* cbw = (struct usb_msc_cbw*)((void*)buf);
        struct usb_msc_csw* csw = (struct usb_msc_csw*)((void*)(buf + USB_BULK_CSW_OFFSET));
        cbw->sig = 0x43425355U;
        cbw->tag = g_usb_msc_tag++;
        if (g_usb_msc_tag == 0) g_usb_msc_tag = 1;
        cbw->data_len = data_len;
        cbw->flags = data_in ? 0x80U : 0x00U;
        cbw->lun = 0;
        cbw->cb_len = cdb_len;
        for (uint8_t i = 0; i < cdb_len; i++) cbw->cb[i] = cdb[i];

        int final_attempt = (attempt == 1);
        if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_out_dci, g_bulk_out_ring_phys, g_bulk_buf_phys, 31, 0, final_attempt) < 0) {
            if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
            return -7;
        }

        if (data_len > 0) {
            if (data_in) {
                memzero((void*)(buf + USB_BULK_DATA_OFFSET), USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET);
                if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_in_dci, g_bulk_in_ring_phys, g_bulk_buf_phys + USB_BULK_DATA_OFFSET,
                                          data_len, 1, final_attempt) < 0) {
                    if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
                    return -8;
                }
                if (data) memcopy(data, (const void*)(buf + USB_BULK_DATA_OFFSET), data_len);
            } else {
                if (data) memcopy((void*)(buf + USB_BULK_DATA_OFFSET), data, data_len);
                if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_out_dci, g_bulk_out_ring_phys, g_bulk_buf_phys + USB_BULK_DATA_OFFSET,
                                          data_len, 0, final_attempt) < 0) {
                    if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
                    return -9;
                }
            }
        }

        memzero((void*)csw, sizeof(*csw));
        if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_in_dci, g_bulk_in_ring_phys, g_bulk_buf_phys + USB_BULK_CSW_OFFSET,
                                  13, 1, final_attempt) < 0) {
            if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
            return -10;
        }
        if (csw->sig != 0x53425355U || csw->tag != cbw->tag || csw->status != 0) {
            if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
            if (csw->sig != 0x53425355U) return -11;
            if (csw->tag != cbw->tag) return -12;
            return -13;
        }

        // Delay to prevent polling starvation / overwhelming the controller
        uint64_t start_tick = USB_NOW_MS();
        while (USB_NOW_MS() - start_tick < 1) {
            USB_CPU_RELAX();
        }

        return 0;
    }
    return -14;
}

static int usb_msc_scsi_inquiry(void) {
    uint8_t cdb[6];
    uint8_t resp[36];
    memzero(cdb, sizeof(cdb));
    memzero(resp, sizeof(resp));
    cdb[0] = 0x12; // INQUIRY
    cdb[4] = sizeof(resp);
    int r = usb_msc_bot_command(cdb, 6, resp, sizeof(resp), 1);
    if (r < 0) return r;
    g_usb_msc_inquiry_ok = 1;
    return 0;
}

static int usb_msc_scsi_read_capacity10(void) {
    uint8_t cdb[10];
    uint8_t resp[8];
    memzero(cdb, sizeof(cdb));
    memzero(resp, sizeof(resp));
    cdb[0] = 0x25; // READ CAPACITY(10)
    int r = usb_msc_bot_command(cdb, 10, resp, sizeof(resp), 1);
    if (r < 0) return r;
    g_usb_block_count =
        ((uint32_t)resp[0] << 24) |
        ((uint32_t)resp[1] << 16) |
        ((uint32_t)resp[2] << 8) |
        (uint32_t)resp[3];
    g_usb_block_count += 1U;
    g_usb_block_size =
        ((uint32_t)resp[4] << 24) |
        ((uint32_t)resp[5] << 16) |
        ((uint32_t)resp[6] << 8) |
        (uint32_t)resp[7];
    g_usb_msc_capacity_ok = (g_usb_block_size != 0);
    return g_usb_msc_capacity_ok ? 0 : -1;
}

static int usb_msc_scsi_test_unit_ready(void) {
    uint8_t cdb[6];
    memzero(cdb, sizeof(cdb));
    cdb[0] = 0x00; // TEST UNIT READY
    return usb_msc_bot_command(cdb, 6, 0, 0, 1);
}

static int usb_msc_scsi_read10(uint32_t lba, void* data, uint32_t block_count) {
    uint8_t cdb[10];
    uint32_t data_len;
    if (!data || block_count == 0) return -1;
    if (!g_usb_msc_capacity_ok || g_usb_block_size == 0) return -2;
    data_len = block_count * g_usb_block_size;
    if (block_count > 0xFFFFU) return -3;
    if (data_len > USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET) return -4;

    memzero(cdb, sizeof(cdb));
    cdb[0] = 0x28; // READ(10)
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[7] = (uint8_t)(block_count >> 8);
    cdb[8] = (uint8_t)(block_count);
    return usb_msc_bot_command(cdb, 10, data, data_len, 1);
}

static int usb_try_qemu_bulk_fallback(uint8_t slot_id) {
    static const struct {
        uint8_t out_ep;
        uint8_t in_ep;
    } candidates[] = {
        { 0x01, 0x81 },
        { 0x01, 0x82 },
        { 0x02, 0x83 },
        { 0x02, 0x81 },
        { 0x02, 0x82 },
    };

    for (uint32_t i = 0; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        g_usb_bulk_out_ep = candidates[i].out_ep;
        g_usb_bulk_in_ep = candidates[i].in_ep;
        if (g_usb_port_speed >= 4) {
            g_usb_bulk_out_mps = 1024;
            g_usb_bulk_in_mps = 1024;
        } else {
            g_usb_bulk_out_mps = 512;
            g_usb_bulk_in_mps = 512;
        }
        g_usb_bulk_out_dci = xhci_dci_from_epaddr(g_usb_bulk_out_ep);
        g_usb_bulk_in_dci = xhci_dci_from_epaddr(g_usb_bulk_in_ep);
        puts("[usb] trying QEMU bulk pair out=0x");
        puthex(g_usb_bulk_out_ep);
        puts(" in=0x");
        puthex(g_usb_bulk_in_ep);
        puts("\r\n");
        if (xhci_setup_bulk_endpoints(slot_id) == 0) {
            g_usb_msc_inquiry_ok = 0;
            g_usb_msc_capacity_ok = 0;
            if (usb_msc_scsi_inquiry() == 0 && usb_msc_scsi_read_capacity10() == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static int xhci_setup_rings(volatile uint8_t* cap, volatile uint8_t* op, uint32_t hcsparams1, uint32_t hcsparams2) {
    const uint32_t max_slots = hcsparams1 & 0xFF;

    // 1) Allocate and clear DCBAA.
    void* dcbaa_page = usb_alloc_dma(1);
    if (!dcbaa_page) return -1;
    memzero(USB_VIRT(dcbaa_page), PAGE_SIZE);
    g_dcbaap_phys = (uint64_t)dcbaa_page;
    volatile uint64_t* dcbaa = (volatile uint64_t*)USB_VIRT(dcbaa_page);

    /* Scratchpad buffer support (required when Max Scratchpad Buffers > 0).
     *
     * ★ **HCSPARAMS2 のビット配置は直感に反する。Hi のほうが下にある。**
     *
     *     bits 25:21   Max Scratchpad Bufs **Hi**
     *     bits 31:27   Max Scratchpad Bufs **Lo**
     *
     * ここを逆に読んでいて **32 倍に見積もっていた** (2026-08-16)。
     * 実機の VL805 は hcsparams2 = 0xfc000031 で、
     *
     *     Lo(31:27) = 31 / Hi(25:21) = 0  ->  count = 31
     *
     * **正しくは 31 個。992 個ではない。** 3.9MB を無駄に確保していた。
     * 上下を入れ替えるだけなので**間違えても動いてしまう** — 実機の
     * 値で桁を確かめること。 */
    uint32_t sp_hi = (hcsparams2 >> 21) & 0x1F;
    uint32_t sp_lo = (hcsparams2 >> 27) & 0x1F;
    uint32_t sp_count = (sp_hi << 5) | sp_lo;
    g_scratchpad_count = sp_count;
    g_scratchpad_array_phys = 0;
    if (sp_count > 0) {
        /* ★ **配列は 1 ページでは足りないことがある。**
         *
         * 1 個あたり 8 バイトなので、512 個を超えると 4KB を溢れる。
         * **VL805 (Raspberry Pi 4) は 992 個を要求する** — 7936 バイト。
         * 1 ページで確保すると直後の領域を壊し、コントローラが配列の
         * 後半でゴミの番地を掴む。
         *
         * **QEMU の xhci は 0 個なので、この経路を一度も通らなかった。**
         * 実機で初めて露見した (2026-08-16、command ring が動かない形で) */
        uint32_t sp_array_bytes = sp_count * 8U;
        uint32_t sp_array_pages = (sp_array_bytes + PAGE_SIZE - 1U) / PAGE_SIZE;
        void* sp_array_page = usb_alloc_dma(sp_array_pages);
        if (!sp_array_page) return -1;
        memzero(USB_VIRT(sp_array_page), (uint64_t)sp_array_pages * PAGE_SIZE);
        g_scratchpad_array_phys = (uint64_t)sp_array_page;

        volatile uint64_t* sp_array = (volatile uint64_t*)USB_VIRT(sp_array_page);
        for (uint32_t i = 0; i < sp_count; i++) {
            void* sp_buf = usb_alloc_dma(1);
            if (!sp_buf) return -1;
            memzero(USB_VIRT(sp_buf), PAGE_SIZE);
            sp_array[i] = (uint64_t)sp_buf;
        }
        dcbaa[0] = g_scratchpad_array_phys;
    }

    // 2) Allocate command ring page (TRBs). Link TRB at tail points to head.
    void* cmd_page = usb_alloc_dma(1);
    if (!cmd_page) return -1;
    memzero(USB_VIRT(cmd_page), PAGE_SIZE);
    g_cmd_ring_phys = (uint64_t)cmd_page;
    volatile uint32_t* cmd = (volatile uint32_t*)USB_VIRT(cmd_page);
    // 256 TRBs per 4KiB. Reserve the last one as Link TRB.
    const uint32_t link_index = 255;
    uint64_t cmd_ring_target = g_cmd_ring_phys & ~0xFULL;
    cmd[link_index * 4 + 0] = (uint32_t)(cmd_ring_target & 0xFFFFFFFFU);
    cmd[link_index * 4 + 1] = (uint32_t)(cmd_ring_target >> 32);
    cmd[link_index * 4 + 2] = 0;
    // Link TRB (type=6) + Toggle Cycle(bit1) + Cycle(bit0=1 at init PCS).
    cmd[link_index * 4 + 3] = (6U << 10) | (1U << 1) | 1U;

    // 3) Allocate one ERST entry and one Event Ring page.
    void* erst_page = usb_alloc_dma(1);
    void* evt_page = usb_alloc_dma(1);
    if (!erst_page || !evt_page) return -1;
    memzero(USB_VIRT(erst_page), PAGE_SIZE);
    memzero(USB_VIRT(evt_page), PAGE_SIZE);
    g_erst_phys = (uint64_t)erst_page;
    g_event_ring_phys = (uint64_t)evt_page;

    volatile uint32_t* erst = (volatile uint32_t*)USB_VIRT(erst_page);
    const uint32_t erst_size = XHCI_EVT_RING_TRBS; // number of TRBs in event ring segment
    erst[0] = (uint32_t)(g_event_ring_phys & 0xFFFFFFFFU);
    erst[1] = (uint32_t)(g_event_ring_phys >> 32);
    erst[2] = erst_size; // Ring Segment Size
    erst[3] = 0;

    /* **リングと ERST を書き終えてからレジスタに番地を教える。**
     * 順序が逆になると、xHC は中身が入る前のメモリを読みに行く */
    USB_MB();

    // 4) Program DCBAAP, CRCR, runtime interrupter-0 ERST registers.
    mmio_write64(op, 0x30, g_dcbaap_phys);
    // CRCR: dequeue ptr + RCS(bit0)=1
    mmio_write64(op, 0x18, (g_cmd_ring_phys & ~0x3FULL) | 1ULL);
    // CONFIG.MaxSlotsEn
    mmio_write32(op, 0x38, (max_slots > 8) ? 8 : max_slots);

    volatile uint8_t* rt = cap + (g_xhci_rtsoff & ~0x1FU);
    // Interrupter 0 register set starts at runtime + 0x20.
    const uint32_t intr0 = 0x20;
    mmio_write32(rt, intr0 + 0x08, 1); // ERSTSZ
    mmio_write64(rt, intr0 + 0x10, g_erst_phys); // ERSTBA
    // ERDP points to first event TRB.
    mmio_write64(rt, intr0 + 0x18, (g_event_ring_phys & ~0xFULL));
    mmio_write32(rt, intr0 + 0x04, 0); // IMOD
    mmio_write32(rt, intr0 + 0x00, (1U << 1)); // IMAN.IE

    g_xhci_rings_ready = 1;
    g_cmd_enqueue_idx = 0;
    g_cmd_cycle = 1;
    g_evt_dequeue_idx = 0;
    g_evt_cycle = 1;
    g_usb_msc_tag = 1;
    return 0;
}

int usb_is_ready(void) {
    return g_usb_ready;
}

int usb_mass_storage_ready(void) {
    return g_usb_mass_ready;
}

int usb_block_device_ready(void) {
    return g_usb_msc_bot_ready && g_usb_msc_capacity_ok;
}

int usb_read_block(uint32_t lba, void* buf, uint32_t count) {
    if (!buf || count == 0) return -1;
    if (!usb_block_device_ready()) return -2;
    if (g_usb_block_size == 0 || count > ((USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET) / g_usb_block_size)) return -3;
    if (lba >= g_usb_block_count) return -4;
    if (lba + count > g_usb_block_count) return -5;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (usb_msc_scsi_read10(lba, buf, count) == 0) return 0;
        (void)usb_msc_recover_bulk_path();
        (void)usb_msc_scsi_test_unit_ready();
    }
    return -6;
}

/* ---- PORTSC を書くときのビット ------------------------------------------
 *
 * **読んだ値をそのまま書き戻してはいけない。**
 *
 *   PED (bit 1) は RW1C。**1 を書くとポートが無効になる。**接続済みの
 *   ポートを読むと PED は 1 なので、read-modify-write すると
 *   「有効にしようとして無効にする」ことになる。
 *
 *   bit 17..23 (CSC/PEC/WRC/OCC/PRC/PLC/CEC) も RW1C で、書き戻すと
 *   まだ処理していない状態変化を勝手に落とす。
 *
 * **持ち回してよいのは RWS のビットだけ。** PLS(5..8) / PP(9) /
 * PIC(14,15) / WCE,WDE,WOE(25..27) */
#define PORTSC_KEEP  ((0xFU << 5) | (1U << 9) | (3U << 14) | (7U << 25))
#define PORTSC_CCS   (1U << 0)   /* Current Connect Status (RO) */
#define PORTSC_PED   (1U << 1)   /* Port Enabled/Disabled */
#define PORTSC_PR    (1U << 4)   /* Port Reset  — USB2 はこちら */
#define PORTSC_CSC   (1U << 17)
#define PORTSC_PRC   (1U << 21)  /* Port Reset Change */
#define PORTSC_WPR   (1U << 31)  /* Warm Port Reset — USB3 はこちら */

static uint32_t xhci_portsc_off(uint8_t port) {
    return 0x400U + ((uint32_t)port - 1U) * 0x10U;
}

/* ---- B-1: xECP の Supported Protocol Capability を読む ---------------------
 *
 * HCCPARAMS1 の bits 31:16 が拡張ケーパビリティへの入口 (dword 単位)。
 * そこから片方向リストを辿り、**ID = 2 (Supported Protocol)** を拾う。
 *
 *   dword0: 31:24 メジャー / 23:16 マイナー / 15:8 次へのオフセット / 7:0 ID
 *   dword1: 名前 ("USB ")
 *   dword2: 15:8 Compatible Port Count / 7:0 Compatible Port Offset
 *
 * **ポート番号は Offset から Count 本ぶん**が、そのメジャー番号になる */
static void xhci_scan_protocols(volatile uint8_t* cap, uint32_t hccparams1) {
    uint32_t off = ((hccparams1 >> 16) & 0xFFFFU) * 4U;
    uint32_t guard = 0;

    for (guard = 0; off != 0 && guard < 64U; guard++) {
        uint32_t d0 = mmio_read32(cap, off);
        uint32_t id = d0 & 0xFFU;
        uint32_t next = (d0 >> 8) & 0xFFU;

        if (id == 2U) {
            uint32_t d1 = mmio_read32(cap, off + 4U);
            uint32_t d2 = mmio_read32(cap, off + 8U);
            uint32_t major = (d0 >> 24) & 0xFFU;
            uint32_t minor = (d0 >> 16) & 0xFFU;
            uint32_t p_off = d2 & 0xFFU;
            uint32_t p_cnt = (d2 >> 8) & 0xFFU;
            uint32_t i;

            puts("[usb] xecp proto USB");
            putdec(major);
            puts(".");
            putdec(minor);
            puts(" ports ");
            putdec(p_off);
            puts("..");
            putdec(p_off + p_cnt - 1U);
            puts(" name=");
            puthex(d1);
            puts("\r\n");

            for (i = 0; i < p_cnt; i++) {
                uint32_t p = p_off + i;
                if (p >= 1U && p < XHCI_MAX_PORTS_TRACKED) {
                    g_port_major[p] = (uint8_t)major;
                }
            }
        }

        if (next == 0) break;
        off += next * 4U;
    }
}

/* ---- B-2: ポートをリセットして有効にする ----------------------------------
 *
 * **接続があるだけでは Address Device は通らない。**ポートを Enabled に
 * するにはリセットが要る。USB2 は PR、USB3 は WPR (Warm Port Reset)。
 *
 * 成功の判定は **PED が 1 になったか**。PRC が立つのはリセットが終わった
 * 合図でしかなく、失敗しても立つ。
 *
 * 戻り値: 0 = 有効になった / -1 = ならなかった */
static int xhci_port_reset(volatile uint8_t* op, uint8_t port) {
    uint32_t o = xhci_portsc_off(port);
    uint32_t portsc = mmio_read32(op, o);
    uint32_t major = (port < XHCI_MAX_PORTS_TRACKED) ? g_port_major[port] : 0U;
    uint32_t rst = (major == 3U) ? PORTSC_WPR : PORTSC_PR;
    uint32_t i;

    if (portsc & PORTSC_PED) return 0; /* もう有効 */

    /* **RWS のビットだけ持ち回してリセットを立てる。**
     * 読んだ値をそのまま書くと PED を叩いて逆効果になる */
    mmio_write32(op, o, (portsc & PORTSC_KEEP) | rst);
    USB_MB();

    /* リセットが落ちるまで待つ。実機の USB2 は 10ms 以上かかる */
    for (i = 0; i < 2000000U; i++) {
        portsc = mmio_read32(op, o);
        if ((portsc & rst) == 0) break;
        USB_CPU_RELAX();
    }

    /* 変化フラグ (PRC / CSC) を落とす。**RW1C なので 1 を書く** */
    mmio_write32(op, o, (portsc & PORTSC_KEEP) | PORTSC_PRC | PORTSC_CSC);
    USB_MB();

    portsc = mmio_read32(op, o);
    puts("[usb] port ");
    putdec(port);
    puts(" (USB");
    putdec(major);
    puts(") reset -> portsc=");
    puthex(portsc);
    puts((portsc & PORTSC_PED) ? "  有効になった\r\n" : "  *** 有効にならなかった\r\n");

    return (portsc & PORTSC_PED) ? 0 : -1;
}

void usb_init(void) {
    g_usb_ready = 0;
    g_usb_mass_ready = 0;
    g_xhci_rings_ready = 0;
    g_xhci_cmd_ready = 0;
    g_xhci_addr_ready = 0;
    g_xhci_cfg_ready = 0;
    g_usb_desc_ready = 0;
    g_usb_cfg_desc_ready = 0;
    g_usb_msc_if_ready = 0;
    g_usb_msc_bot_ready = 0;
    g_usb_msc_inquiry_ok = 0;
    g_usb_msc_capacity_ok = 0;
    g_usb_vid = 0;
    g_usb_pid = 0;
    g_usb_dev_class = 0;
    g_usb_dev_subclass = 0;
    g_usb_dev_proto = 0;
    g_usb_msc_if_class = 0;
    g_usb_msc_if_subclass = 0;
    g_usb_msc_if_proto = 0;
    g_usb_cfg_value = 0;
    g_usb_msc_if_number = 0;
    g_usb_bulk_out_ep = 0;
    g_usb_bulk_in_ep = 0;
    g_usb_ep0_mps = 64;
    g_usb_bulk_out_mps = 0;
    g_usb_bulk_in_mps = 0;
    g_usb_bulk_out_dci = 0;
    g_usb_bulk_in_dci = 0;
    g_usb_block_count = 0;
    g_usb_block_size = 0;
    g_xhci_slot_id = 0;
    g_xhci_port_id = 0;
    g_xhci_mmio = 0;

    /* **確保より前に決めること。** 後だと、先に確保したものだけ
     * 変換されないまま残る */
    g_usb_dma_offset = usb_arch_dma_offset();
    if (g_usb_dma_offset) {
        puts("[usb] dma offset 0x");
        puthex(g_usb_dma_offset);
        puts("\r\n");
    }

    /* **アーキ側が別の経路で見つけていたら、そちらを使う。**
     *
     * Raspberry Pi 4 の xHCI (VL805) は PCIe の先にいて、設定空間が
     * ECAM ではない (索引とデータの 2 段)。そちらは
     * kernel/aarch64/pcie_brcm.c が面倒を見るので、ここへは**結果の
     * MMIO 番地だけ**が渡ってくる。0 なら従来どおり PCI から探す */
    uint64_t mmio_phys = usb_arch_xhci_mmio();
    if (mmio_phys == 0) {
        struct pci_device_info xhci;
        if (pci_find_xhci(&xhci) < 0) {
            puts("[usb] no xHCI controller\r\n");
            return;
        }
        pci_enable_mmio_busmaster(&xhci);

        mmio_phys = pci_get_bar0_mmio(&xhci);
        if (mmio_phys == 0) {
            puts("[usb] xHCI BAR0 invalid\r\n");
            return;
        }

        /* **4GiB 以上は HHDM に無い。**PCI から見つけた場合は張られて
         * いないので断る。**アーキ側が渡してきた場合は別** — あちらは
         * 自分で張ったうえで渡してくる (Pi 4 の BAR は 0x6_00000000) */
        if (mmio_phys >= 0x100000000ULL) {
            puts("[usb] xHCI BAR0 above 4GiB is not mapped yet\r\n");
            puthex(mmio_phys);
            puts("\r\n");
            return;
        }
    }

    /* **ここは MMIO。DMA の窓とは無関係なので USB_VIRT を使わない** */
    volatile uint8_t* cap = (volatile uint8_t*)PHYS_TO_VIRT_MMIO(mmio_phys);
    g_cap_regs = cap;
    uint8_t caplen = cap[0];
    uint16_t hciversion = (uint16_t)cap[2] | ((uint16_t)cap[3] << 8);
    uint32_t hcsparams1 = mmio_read32(cap, 0x04);
    uint32_t hcsparams2 = mmio_read32(cap, 0x08);
    uint32_t hccparams1 = mmio_read32(cap, 0x10);
    g_xhci_max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    g_xhci_dboff = mmio_read32(cap, 0x14);
    g_xhci_rtsoff = mmio_read32(cap, 0x18);
    g_xhci_ctx_size = (hccparams1 & (1U << 2)) ? 64U : 32U;

    /* **どのポートが USB2 でどれが USB3 かを確定させる。**MaxPorts からは
     * 決められない (B-1)。リセットの手順が種類で違うので先に読む */
    xhci_scan_protocols(cap, hccparams1);

    // xHCI bring-up: stop -> reset -> setup rings -> run
    volatile uint8_t* op = cap + caplen;
    g_op_regs = op;
    g_db_regs = cap + (g_xhci_dboff & ~0x3U);
    g_rt_regs = cap + (g_xhci_rtsoff & ~0x1FU);
    int cmd_probe = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t usbcmd = mmio_read32(op, 0x00);
        mmio_write32(op, 0x00, usbcmd & ~1U); // RS=0 (stop)
        (void)xhci_wait_bits(op, 0x04, (1U << 0), (1U << 0), 500000); // USBSTS.HCHalted=1

        usbcmd = mmio_read32(op, 0x00);
        mmio_write32(op, 0x00, usbcmd | (1U << 1)); // HCRST=1
        if (xhci_wait_bits(op, 0x00, (1U << 1), 0, 2000000) < 0) {
            puts("[usb] xHCI reset timeout\r\n");
            return;
        }
        // Wait for Controller Not Ready (CNR, USBSTS bit11) to clear.
        if (xhci_wait_bits(op, 0x04, (1U << 11), 0, 2000000) < 0) {
            puts("[usb] xHCI CNR timeout\r\n");
            return;
        }

        if (xhci_setup_rings(cap, op, hcsparams1, hcsparams2) < 0) {
            puts("[usb] xHCI ring setup failed\r\n");
            return;
        }

        /* **PAGESIZE は読み出し専用。**書いても意味が無いので読んで確かめる。
         * bit n が立っていれば 2^(n+12) バイトを扱えるという意味で、
         * bit0 = 4KB。**4KB が使えない機械なら、こちらのリング確保が
         * そもそも合っていない**ので、その場で分かるようにしておく */
        {
            uint32_t pagesize = mmio_read32(op, 0x08);
            if (!(pagesize & 1U)) {
                puts("[usb] *** PAGESIZE=0x");
                puthex(pagesize);
                puts(" — 4KB が使えない。リングの確保が合っていない\r\n");
            }
        }

        // Clear pending status bits before Run.
        mmio_write32(op, 0x04, 0xFFFFFFFFU);

        usbcmd = mmio_read32(op, 0x00);
        // Run + INTE (interrupts); events are still polled path for now.
        mmio_write32(op, 0x00, usbcmd | 1U | (1U << 2)); // RS=1, INTE=1
        if (xhci_wait_bits(op, 0x04, (1U << 0), 0, 2000000) < 0) {
            puts("[usb] xHCI run timeout\r\n");
            return;
        }

        cmd_probe = xhci_cmd_noop();
        if (cmd_probe == 0) {
            g_xhci_cmd_ready = 1;
            break;
        }

        if (attempt == 0) {
            puts("[usb] xHCI command probe retry\r\n");
        }
    }

    if (!g_xhci_cmd_ready) {
        puts("[usb] xHCI command ring probe failed code=");
        puthex((uint64_t)(uint32_t)(-cmd_probe));
        puts("\r\n");
    } else {
        uint8_t slot_id = 0;
        int es = xhci_cmd_enable_slot(&slot_id);
        if (es == 0) {
            g_xhci_slot_id = slot_id;
            puts("[usb] Enable Slot OK slot=");
            puthex(slot_id);
            puts("\r\n");

            /* **接続のあるポートを、リセットして有効にしてから選ぶ。**
             * 接続があるだけでは Address Device は通らない (実機で
             * TRB Error になった)。全ポートの生の PORTSC も出しておく —
             * 無いと失敗の理由を推測でしか言えない */
            for (uint8_t p = 1; p <= g_xhci_max_ports; p++) {
                uint32_t portsc = mmio_read32(op, xhci_portsc_off(p));
                puts("[usb] portsc[");
                putdec(p);
                puts("]=");
                puthex(portsc);
                puts(" USB");
                putdec((p < XHCI_MAX_PORTS_TRACKED) ? g_port_major[p] : 0U);
                puts((portsc & PORTSC_CCS) ? "  接続あり\r\n" : "\r\n");
            }
            for (uint8_t p = 1; p <= g_xhci_max_ports; p++) {
                uint32_t portsc = mmio_read32(op, xhci_portsc_off(p));
                if (!(portsc & PORTSC_CCS)) continue;
                if (xhci_port_reset(op, p) == 0) {
                    g_xhci_port_id = p;
                    break;
                }
            }
            if (g_xhci_port_id != 0) {
                int ad = xhci_cmd_address_device(g_xhci_slot_id, g_xhci_port_id);
                if (ad == 0) {
                    g_xhci_addr_ready = 1;
                    puts("[usb] Address Device OK port=");
                    puthex(g_xhci_port_id);
                    puts("\r\n");

                    int ce = xhci_cmd_configure_endpoint(g_xhci_slot_id);
                    if (ce == 0) {
                        g_xhci_cfg_ready = 1;
                        puts("[usb] Configure Endpoint OK slot=");
                        puthex(g_xhci_slot_id);
                        puts("\r\n");
                    } else {
                        // For default EP0 path, many controllers allow proceeding
                        // after Address Device even if Configure Endpoint is rejected.
                        g_xhci_cfg_ready = 1;
                        puts("[usb] Configure Endpoint skipped code=");
                        puthex((uint64_t)(uint32_t)(-ce));
                        puts(" (continue with EP0)\r\n");
                    }

                    int gd = xhci_ep0_get_device_descriptor(g_xhci_slot_id);
                    if (gd == 0) {
                        puts("[usb] GET_DESCRIPTOR(Device) OK vid=0x");
                        puthex(g_usb_vid);
                        puts(" pid=0x");
                        puthex(g_usb_pid);
                        puts(" class=0x");
                        puthex(g_usb_dev_class);
                        puts("\r\n");
                    } else {
                        puts("[usb] GET_DESCRIPTOR(Device) failed code=");
                        puthex((uint64_t)(uint32_t)(-gd));
                        puts("\r\n");
                    }

                    /* ---- B-3: 相手がハブなら、その先へ進む ------------------
                     *
                     * **Raspberry Pi 4 のルートポートに居るのはハブ 1 台だけ。**
                     * ここで止まると、キーボードには永久に届かない */
                    if (gd == 0 && g_usb_dev_class == 0x09) {
                        uint8_t nbr = 0, mtt = 0, hub_port = 0;
                        uint32_t dev_speed = 0;

                        g_hub_slot_id = g_xhci_slot_id;
                        puts("[usb] ハブを見つけた。その先を探す\r\n");

                        /* **ポート要求の前に設定を選ぶ。**Address 状態のまま
                         * SET_FEATURE を投げると実機の VL805 は STALL する */
                        {
                            int hc = usb_hub_configure(g_hub_slot_id);
                            puts("[usb] hub SET_CONFIGURATION ");
                            if (hc == 0) {
                                puts("ok\r\n");
                            } else {
                                puts("*** 失敗 code=");
                                puthex((uint64_t)(uint32_t)(-hc));
                                puts("\r\n");
                                (void)xhci_ep0_recover_stall(g_hub_slot_id);
                            }
                        }

                        if (usb_hub_get_descriptor(g_hub_slot_id, &nbr, &mtt) == 0) {
                            g_hub_nbr_ports = nbr;
                            puts("[usb] hub ports=");
                            putdec(nbr);
                            puts("\r\n");

                            if (usb_hub_mark_slot(g_hub_slot_id, nbr, mtt) < 0) {
                                puts("[usb] *** スロットに Hub ビットを立てられなかった\r\n");
                            }

                            if (usb_hub_find_device(g_hub_slot_id, nbr, &hub_port, &dev_speed) == 0) {
                                uint8_t dev_slot = 0;
                                /* 高速ハブの先の低速/全速デバイスは TT が要る */
                                uint8_t tt_slot = (dev_speed < 3) ? g_hub_slot_id : 0;
                                uint8_t tt_port = (dev_speed < 3) ? hub_port : 0;

                                /* **ハブの文脈をここで控える。**
                                 * この後 Address Device が g_ep0_* を子デバイスの
                                 * ものに書き換える。**リングの位置も一緒に控える** —
                                 * ポートを探す間にハブのリングは進んでいるので、
                                 * 古い位置を戻すと xHC が見ない場所に TRB を書くことになる */
                                usb_ctx_store(USB_CTX_HUB);

                                if (xhci_cmd_enable_slot(&dev_slot) == 0) {
                                    uint32_t route = (uint32_t)hub_port & 0xFU; /* Route String 1 段目 */
                                    int ad2 = xhci_cmd_address_device_full(
                                        dev_slot, g_xhci_port_id, route,
                                        dev_speed, tt_slot, tt_port, 0);
                                    puts("[usb] hub 先 Address Device slot=");
                                    putdec(dev_slot);
                                    puts(" port=");
                                    putdec(hub_port);
                                    puts(" speed=");
                                    putdec(dev_speed);
                                    if (ad2 == 0) {
                                        g_xhci_slot_id = dev_slot;
                                        puts("  ok\r\n");

                                        /* ---- 全速の相手は EP0 の MPS を後から入れ替える ----
                                         *
                                         * **EP0 の最大パケット長は繋いでみるまで
                                         * 分からない** (8/16/32/64)。8 と決め打ちで
                                         * 先頭 8 バイトだけ読んで確かめ、
                                         * **Evaluate Context (A1) で入れ替える。**
                                         *
                                         * **8 バイトで打ち切ることは相手を壊さない。**
                                         * wLength=8 と言って 8 バイト受け取るのは
                                         * 標準的な列挙手順で、Linux の hub_port_init()
                                         * も同じことをする。以前ここに書いていた
                                         * 「10 バイト残って同期が外れるのでポートを
                                         * リセットしてやり直す」は仕様と逆だった
                                         * (2026-08-18 codex 相談で訂正)。
                                         *
                                         * ---- 探針 A / B / C (2026-08-18) --------
                                         *
                                         * **落ちたらそこで止める。**STALL は EP0 を
                                         * Halted にするので、続けると以後の結果が
                                         * 混ざって読めなくなる */
                                        uint32_t learned_mps = 0;
                                        if (dev_speed < 3U) {
                                            int ok = (usb_probe_desc("基準", dev_slot, 8) == 0);
                                            if (ok) {
                                                volatile uint8_t* d =
                                                    (volatile uint8_t*)USB_VIRT((void*)g_ep0_buf_phys);
                                                uint32_t m = d[7];
                                                if (d[1] != 1 || (m != 8 && m != 16 && m != 32 && m != 64)) {
                                                    puts("[pb] 基準の中身が記述子でない\r\n");
                                                    ok = 0;
                                                } else {
                                                    learned_mps = m;
                                                }
                                            }
                                            /* A: リセットも MPS 変更もしていない状態で、
                                             *    同じ要求をもう一度 */
                                            if (ok) ok = (usb_probe_desc("A", dev_slot, 8) == 0);
                                            /* MPS を入れ替える */
                                            if (ok && learned_mps != g_usb_ep0_mps) {
                                                int er = xhci_cmd_evaluate_context_mps(dev_slot, learned_mps);
                                                puts("[pb] Evaluate Context mps ");
                                                putdec(g_usb_ep0_mps);
                                                puts(" -> ");
                                                putdec(learned_mps);
                                                puts(er == 0 ? "  ok\r\n" : "  *** 失敗\r\n");
                                                if (er == 0) {
                                                    g_usb_ep0_mps = (uint16_t)learned_mps;
                                                    xhci_dump_dev_ctx("eval後", g_output_ctx_phys);
                                                } else {
                                                    ok = 0;
                                                }
                                            }
                                            /* B: MPS だけ動かした状態で、同じ 8 バイト読み */
                                            if (ok) ok = (usb_probe_desc("B", dev_slot, 8) == 0);
                                            /* C: 本番の 18 バイト読み */
                                            if (ok) ok = (usb_probe_desc("C", dev_slot, 18) == 0);
                                            puts(ok ? "[pb] A/B/C 全部通った\r\n"
                                                    : "[pb] ここで止めた\r\n");
                                        }

                                        /* **記述子を読む前に Configure Endpoint は要らない。**
                                         * Address Device が通った時点で EP0 は Running。
                                         * ハブと違ってポート要求を出すわけでもないので、
                                         * Configured へ進める理由が無い (2026-08-18 codex 相談) */
                                        int gd2 = xhci_ep0_get_device_descriptor_retry(dev_slot, 3);
                                        puts("[usb] hub 先 GET_DESCRIPTOR(Device) ");
                                        if (gd2 == 0) {
                                            puts("OK vid=0x");
                                            puthex(g_usb_vid);
                                            puts(" pid=0x");
                                            puthex(g_usb_pid);
                                            puts(" class=0x");
                                            puthex(g_usb_dev_class);
                                            puts("\r\n");
                                        } else {
                                            puts("failed code=");
                                            puthex((uint64_t)(uint32_t)(-gd2));
                                            puts("\r\n");
                                        }
                                    } else {
                                        puts("  *** 失敗 code=");
                                        puthex((uint64_t)(uint32_t)(-ad2));
                                        puts("\r\n");
                                    }
                                }
                            } else {
                                puts("[usb] ハブのどのポートにも何も繋がっていない\r\n");
                            }
                        } else {
                            puts("[usb] *** ハブ記述子が読めなかった\r\n");
                        }
                    }

                    int gc = xhci_ep0_get_config_descriptor(g_xhci_slot_id);
                    if (gc == 0 && g_usb_hid_if_ready) {
                        puts("[usb] HID keyboard if=0x");
                        puthex(g_usb_hid_if_number);
                        puts(" ep=0x");
                        puthex(g_usb_int_in_ep);
                        puts(" mps=0x");
                        puthex(g_usb_int_in_mps);
                        puts(" interval=0x");
                        puthex(g_usb_int_in_interval);
                        puts(" dci=0x");
                        puthex(g_usb_int_in_dci);
                        puts("\r\n");
                    }
                    if (gc == 0) {
                        puts("[usb] GET_DESCRIPTOR(Config) MSC if class=0x");
                        puthex(g_usb_msc_if_class);
                        puts(" sub=0x");
                        puthex(g_usb_msc_if_subclass);
                        puts(" proto=0x");
                        puthex(g_usb_msc_if_proto);
                        puts(" cfg=0x");
                        puthex(g_usb_cfg_value);
                        puts(" if=0x");
                        puthex(g_usb_msc_if_number);
                        puts(" epout=0x");
                        puthex(g_usb_bulk_out_ep);
                        puts(" epin=0x");
                        puthex(g_usb_bulk_in_ep);
                        puts("\r\n");
                    } else {
                        puts("[usb] GET_DESCRIPTOR(Config) failed code=");
                        puthex((uint64_t)(uint32_t)(-gc));
                        puts("\r\n");
                        // QEMU usb-storage fallback: device class is often 0x00
                        // and interface descriptor fetch may be flaky in early stack.
                        if (g_usb_vid == 0x46F4 && g_usb_pid == 0x0001) {
                            g_usb_msc_if_ready = 1;
                            g_usb_cfg_value = 1;
                            g_usb_msc_if_number = 0;
                            g_usb_msc_if_class = 0x08;
                            g_usb_msc_if_subclass = 0x06;
                            g_usb_msc_if_proto = 0x50;
                            g_usb_bulk_out_ep = 0x01;
                            g_usb_bulk_in_ep = 0x82;
                            g_usb_bulk_out_mps = 64;
                            g_usb_bulk_in_mps = 64;
                            g_usb_bulk_out_dci = xhci_dci_from_epaddr(g_usb_bulk_out_ep);
                            g_usb_bulk_in_dci = xhci_dci_from_epaddr(g_usb_bulk_in_ep);
                            puts("[usb] Applying QEMU MSC fallback (46f4:0001)\r\n");
                        }
                    }
                    if (g_usb_msc_if_ready) {
                        int can_continue = 0;
                        uint64_t set_cfg = 0;
                        set_cfg |= 0x00ULL;
                        set_cfg |= (uint64_t)9 << 8; // SET_CONFIGURATION
                        set_cfg |= (uint64_t)g_usb_cfg_value << 16;
                        int sc = xhci_ep0_control_no_data(g_xhci_slot_id, set_cfg);
                        if (sc == 0) {
                            can_continue = 1;
                        } else if (g_usb_vid == 0x46F4 && g_usb_pid == 0x0001) {
                            puts("[usb] SET_CONFIGURATION failed, continue with QEMU fallback\r\n");
                            can_continue = 1;
                        } else {
                            puts("[usb] SET_CONFIGURATION failed code=");
                            puthex((uint64_t)(uint32_t)(-sc));
                            puts("\r\n");
                        }

                        if (can_continue) {
                            int be;
                            if (!g_usb_cfg_desc_ready && g_usb_vid == 0x46F4 && g_usb_pid == 0x0001) {
                                be = usb_try_qemu_bulk_fallback(g_xhci_slot_id);
                            } else {
                                be = xhci_setup_bulk_endpoints(g_xhci_slot_id);
                            }
                            if (be == 0) {
                                int iq = usb_msc_scsi_inquiry();
                                int rc = usb_msc_scsi_read_capacity10();
                                if (iq == 0 && rc == 0) {
                                    g_usb_msc_bot_ready = 1;
                                    puts("[usb] MSC BOT ready blocks=0x");
                                    puthex(g_usb_block_count);
                                    puts(" block_size=0x");
                                    puthex(g_usb_block_size);
                                    puts("\r\n");
                                } else {
                                    puts("[usb] MSC BOT probe failed inquiry=0x");
                                    puthex((uint64_t)(uint32_t)(-iq));
                                    puts(" capacity=0x");
                                    puthex((uint64_t)(uint32_t)(-rc));
                                    puts("\r\n");
                                }
                            } else {
                                puts("[usb] bulk endpoint setup failed code=");
                                    puthex((uint64_t)(uint32_t)(-be));
                                    puts("\r\n");
                            }
                        }
                    }
                } else {
                    puts("[usb] Address Device failed code=");
                    puthex((uint64_t)(uint32_t)(-ad));
                    puts("\r\n");
                }
            }
        } else {
            puts("[usb] Enable Slot failed code=");
            puthex((uint64_t)(uint32_t)(-es));
            puts("\r\n");
        }
    }

    g_xhci_mmio = mmio_phys;
    g_usb_ready = 1;

    puts("[usb] xHCI capability mapped at phys=0x");
    puthex(mmio_phys);
    puts(" caplen=0x");
    puthex(caplen);
    puts(" hciver=0x");
    puthex(hciversion);
    puts(" hcsparams1=0x");
    puthex(hcsparams1);
    puts(" hcsparams2=0x");
    puthex(hcsparams2);
    puts(" dboff=0x");
    puthex(g_xhci_dboff);
    puts(" rtsoff=0x");
    puthex(g_xhci_rtsoff);
    puts(" cmd=");
    puts(g_xhci_cmd_ready ? "ok" : "fail");
    puts(" addr=");
    puts(g_xhci_addr_ready ? "ok" : "fail");
    puts(" cfg=");
    puts(g_xhci_cfg_ready ? "ok" : "fail");
    puts(" desc=");
    puts(g_usb_desc_ready ? "ok" : "fail");
    puts(" cfgd=");
    puts(g_usb_cfg_desc_ready ? "ok" : "fail");
    puts(" mscif=");
    puts(g_usb_msc_if_ready ? "ok" : "fail");
    puts(" slot=");
    puthex(g_xhci_slot_id);
    puts(" port=");
    puthex(g_xhci_port_id);
    puts(" sp=");
    puthex(g_scratchpad_count);
    puts("\r\n");

    // Probe connected ports. Full enumeration/BOT/SCSI is next phase.
    uint32_t connected = 0;
    uint32_t enabled = 0;
    for (uint8_t p = 0; p < g_xhci_max_ports; p++) {
        uint32_t portsc = mmio_read32(op, 0x400 + (uint32_t)p * 0x10);
        if (portsc & 0x1U) connected++;
        if (portsc & 0x2U) enabled++;
    }
    if (connected > 0 && g_xhci_rings_ready && g_xhci_cmd_ready && g_usb_msc_bot_ready) {
        g_usb_mass_ready = 1;
    }
    puts("[usb] ports: max=");
    puthex(g_xhci_max_ports);
    puts(" connected=");
    puthex(connected);
    puts(" enabled=");
    puthex(enabled);
    puts(" rings=");
    puts(g_xhci_rings_ready ? "ready" : "not-ready");
    puts(" cmd=");
    puts(g_xhci_cmd_ready ? "ok" : "fail");
    puts(" addr=");
    puts(g_xhci_addr_ready ? "ok" : "fail");
    puts(" cfg=");
    puts(g_xhci_cfg_ready ? "ok" : "fail");
    puts(" desc=");
    puts(g_usb_desc_ready ? "ok" : "fail");
    puts(" cfgd=");
    puts(g_usb_cfg_desc_ready ? "ok" : "fail");
    puts(" mscif=");
    puts(g_usb_msc_if_ready ? "ok" : "fail");
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    puts(" slot=");
    puthex(g_xhci_slot_id);
    puts(" port=");
    puthex(g_xhci_port_id);
    puts("\r\n");

    if (!g_usb_mass_ready) {
        puts("[usb] Mass Storage transport is not implemented yet\r\n");
    }
}

void usb_dump_status(void) {
    puts("[usb] status: xhci=");
    puts(g_usb_ready ? "ready" : "not-ready");
    puts(" msc=");
    puts(g_usb_mass_ready ? "ready" : "not-ready");
    puts(" rings=");
    puts(g_xhci_rings_ready ? "ready" : "not-ready");
    puts(" cmd=");
    puts(g_xhci_cmd_ready ? "ok" : "fail");
    puts(" addr=");
    puts(g_xhci_addr_ready ? "ok" : "fail");
    puts(" cfg=");
    puts(g_xhci_cfg_ready ? "ok" : "fail");
    puts(" desc=");
    puts(g_usb_desc_ready ? "ok" : "fail");
    puts(" cfgd=");
    puts(g_usb_cfg_desc_ready ? "ok" : "fail");
    puts(" mscif=");
    puts(g_usb_msc_if_ready ? "ok" : "fail");
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    puts(" slot=");
    puthex(g_xhci_slot_id);
    puts(" port=");
    puthex(g_xhci_port_id);
    puts(" mmio=0x");
    puthex(g_xhci_mmio);
    puts(" dcbaa=0x");
    puthex(g_dcbaap_phys);
    puts(" cr=0x");
    puthex(g_cmd_ring_phys);
    puts(" er=0x");
    puthex(g_event_ring_phys);
    puts(" sp=0x");
    puthex(g_scratchpad_array_phys);
    puts(" inctx=0x");
    puthex(g_input_ctx_phys);
    puts(" outctx=0x");
    puthex(g_output_ctx_phys);
    puts(" ep0=0x");
    puthex(g_ep0_ring_phys);
    puts(" vid=0x");
    puthex(g_usb_vid);
    puts(" pid=0x");
    puthex(g_usb_pid);
    puts(" cls=0x");
    puthex(g_usb_dev_class);
    puts(" sub=0x");
    puthex(g_usb_dev_subclass);
    puts(" pr=0x");
    puthex(g_usb_dev_proto);
    puts(" ifc=0x");
    puthex(g_usb_msc_if_class);
    puts(" ifs=0x");
    puthex(g_usb_msc_if_subclass);
    puts(" ifp=0x");
    puthex(g_usb_msc_if_proto);
    puts(" cfgv=0x");
    puthex(g_usb_cfg_value);
    puts(" ifn=0x");
    puthex(g_usb_msc_if_number);
    puts(" epout=0x");
    puthex(g_usb_bulk_out_ep);
    puts(" epin=0x");
    puthex(g_usb_bulk_in_ep);
    puts(" dciout=0x");
    puthex(g_usb_bulk_out_dci);
    puts(" dciin=0x");
    puthex(g_usb_bulk_in_dci);
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    puts(" iq=");
    puts(g_usb_msc_inquiry_ok ? "ok" : "fail");
    puts(" cap=");
    puts(g_usb_msc_capacity_ok ? "ok" : "fail");
    puts(" blocks=0x");
    puthex(g_usb_block_count);
    puts(" blksz=0x");
    puthex(g_usb_block_size);
    puts("\r\n");
}
