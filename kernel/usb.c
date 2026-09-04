#include <stdint.h>
#include "usb.h"
#include "pci.h"
#include "spinlock.h"   /* irq_save_disable / irq_restore */
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

/* **桁数を指定して 16 進で出す。**puthex は 64bit を丸ごと 16 桁で出すので、
 * vid/pid のような 16bit の値が 0x0000000000001a81 になって読みにくい */
static void puthex_n(uint64_t v, int digits) {
    static const char hex[] = "0123456789abcdef";
    char out[17];
    int i;
    if (digits < 1) digits = 1;
    if (digits > 16) digits = 16;
    for (i = 0; i < digits; i++) out[i] = hex[(v >> ((digits - 1 - i) * 4)) & 0xFU];
    out[digits] = 0;
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

/* ---- 1 行を割らせない囲い (M-2) -------------------------------------------
 *
 * **このファイルは 1 行を puts / putdec の何十回かに分けて組み立てる。**
 * その隙間に別のコアの出力が入ると、実機のログはこうなる:
 *
 *     [ep0] seq=13 slot=1 IN  bReq=0x00 wIndex=1 idx0=  part 39    : type=
 *
 * 「part 39」も「type=」も実在しない。**2 本の行が混ざっているだけ**だが、
 * これを数えて誤読したことが実際にある (日報2026-08-26 の反省)。
 *
 * アーキ側が行の囲いを持っていれば使う。**既定は何もしない** —
 * x86 は大きいカーネルロックの下で出しているので、まずはそのまま */
__attribute__((weak)) void usb_arch_console_begin(void) {}
__attribute__((weak)) void usb_arch_console_end(void) {}

/* **V-1 の計器の既定 (aarch64 以外)。**測っていないので 0 を返す。
 * n=0 のときは要約行を出さない */
__attribute__((weak))
void aarch64_fork_stats(uint64_t* calls, uint64_t* pages, uint64_t* ms) {
    if (calls) *calls = 0;
    if (pages) *pages = 0;
    if (ms) *ms = 0;
}

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
/* **タイマ割り込みとの排他。**イベントリングの取り出し位置は
 * g_evt_dequeue_idx ひとつしかなく、usb_hid_keyboard_poll() (タイマ IRQ)
 * と usb_hotplug_poll() (idle) が同時に進めると壊れる。
 * 割り込みを止めて守るには制御転送は長すぎる (ms 単位) ので、
 * **抜き差しを見ている間はキーボードのポーリングを休ませる**。
 * 休むのは最長でも 500ms に 1 回の確認の間だけ */
static volatile int g_usb_busy = 0;

/* **xHCI を触るのは一度に 1 人だけ (D-5)。**
 *
 * 抜き差しの走査 (usb_hotplug_poll) は**各 CPU の idle ループ**から
 * 呼ばれる (kernel/sched.c:158)。SMP では 4 コアが同時に入る。
 * 500ms のガード (:3382) は「読んで、比べて、書く」なので**揃って
 * 通り抜ける** — その先でコマンドリングとイベントリングを取り合う。
 * このファイル自身が「取り合うと壊れる」と書いている形 (:2611)。
 *
 * g_usb_busy はこれとは別物で、**抜き差し走査からキーボードへの
 * 一方向の合図**にすぎない。立てる前に窓があり、走査どうしは
 * 素通しだった。
 *
 * **待たずに戻る (try)。** idle タスクは割り込みを閉じて走っている
 * (:645) ので、ここでスピンすると閉じたまま相手を待つことになる。
 * どちらも「定期的に見に行く」処理で、**1 回飛ばしても次の周回で
 * 拾える。** */
static volatile uint32_t g_usb_owner = 0;

static int usb_owner_try_take(void) {
    return __sync_bool_compare_and_swap(&g_usb_owner, 0U, 1U);
}

static void usb_owner_release(void) {
    __sync_synchronize();
    g_usb_owner = 0U;
}

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

/* ---- イベントリングと置き場の排他 (U-2) -----------------------------------
 *
 * **`irq_save_disable()` では 4 コアで守れない。**あれが止めるのは自分の
 * CPU の割り込みだけで、他のコアが同時に同じ場所を触るのは止まらない。
 * ここは 1 コアの時代に書かれたまま SMP に乗っていた
 * (このファイル自身が「並行性は未対処」と書き残している)。
 *
 * 同時に入る者は 3 通り:
 *
 *   usb_xhci_irq()      CPU 0 の割り込み。xhci_evt_pump を呼ぶ
 *   待ち手の保険        任意のコアのタスク文脈。同じ pump を呼ぶ
 *   xhci_evt_stash_take 任意のコア。置き場を詰め替える
 *
 * 重なると **g_evt_dequeue_idx を 2 人で進めて、イベントを 1 つ飛ばす。**
 * 実機の症状はこれ:
 *
 *   [usb] ep0 IN *** イベントが来ない slot=1 seq=860 ...
 *   [recov] 完了イベントが来ない
 *
 * **大きい所有権 (g_usb_owner) を割り込みに取らせるのは駄目。**あちらは
 * ミリ秒かかる制御転送の全体を囲んでいて、割り込みがそこで待つと線が
 * 塞がる (日報2026-08-26 の U-2 が「try では足りない、設計から」と
 * 書いていたのはこの点)。
 *
 * **そこで、リングと置き場だけを囲む短いロックを別に持つ。**囲むのは
 * 「吸い出す」「取り出す」「捨てる」の 3 つだけで、制御転送は入らない。
 * 割り込みが待たされるのは他のコアの吸い出し 1 回ぶん = マイクロ秒。
 *
 * **必ず irqsave 版を使う。**自分の CPU の割り込みを閉じないと、
 * 握ったまま割り込まれて自分で自分を待つ。 */
static spinlock_t g_evt_lock;
static uint32_t g_evt_cycle = 1;
static uint64_t g_input_ctx_phys = 0;
static uint64_t g_output_ctx_phys = 0;
static uint64_t g_ep0_buf_phys = 0;
static uint64_t g_ep0_cfg_buf_phys = 0;

/* ---- EP0 のリングはスロットごと ------------------------------------------
 *
 * **1 本のグローバルで持っていたのが 2026-08-20 の詰まりの原因だった。**
 * Address Device のたびに上書きされるので、ハブ (slot 1) を掴んだあとに
 * キーボード (slot 2) を掴むと、**ハブ宛の制御転送がキーボードのリングに
 * TRB を積んでハブのドアベルを叩く**。ハブのデバイス文脈に登録されている
 * リングには何も来ないので、転送イベントが返らず黙ってタイムアウトする。
 * 起動時の列挙が通っていたのは、キーボードを掴む前だったから。
 *
 * **slot_id は uint8_t なので、256 個持てば範囲外が起きない。**
 * 4KB の .bss と引き換えに、添字の確認を全部無くす */
#define XHCI_EP0_SLOTS 256
static struct {
    uint64_t ring_phys;
    uint32_t enq_idx;
    uint32_t cycle;
} g_ep0_slot[XHCI_EP0_SLOTS];
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

/* **一周したら Link TRB のサイクルビットを書き換える。**
 *
 * xHC は TRB のサイクルビットが自分の CCS と一致する間だけ消費する。
 * Link TRB は xhci_ring_reset() が cycle=1 で置いたきり更新していなかった
 * ので、2 周目 (積む側のサイクルが 0) に入ると **Link のところで
 * 一致しなくなり、xHC がそこで止まる**。そのエンドポイントは以後
 * 永久に沈黙する。
 *
 * 規格どおりの手順は「Link TRB のサイクルを**今の** PCS に合わせて
 * 書いてから、TC=1 なので PCS を反転する」。
 *
 * 2026-08-20 の実機で嵌まった。抜き差しのポーリングが 500ms ごとに
 * 12 TRB 積むので 10 秒ほどで一周し、そこから GET_STATUS が返らなく
 * なった。EP0 だけでなく**割り込みリングもバルクも同じ関数を使って
 * いる** — キーボードも 255 回ぶん積めば同じように黙る */
static void xhci_ring_enqueue_advance(uint64_t ring_phys, uint32_t* idx,
                                      uint32_t* cycle, uint32_t count) {
    while (count--) {
        (*idx)++;
        if (*idx == 255) {
            if (ring_phys) {
                volatile uint32_t* ring =
                    (volatile uint32_t*)USB_VIRT((void*)ring_phys);
                uint32_t d3 = ring[255 * 4 + 3];
                ring[255 * 4 + 3] = (d3 & ~1U) | (*cycle & 1U);
                USB_MB();   /* xHC に見せてから自分のサイクルを反転する */
            }
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
static void xhci_evt_stash_clear(void);   /* 定義は下の「イベントの置き場」 */

static uint32_t xhci_evt_drain(void) {
    uint32_t d[4];
    uint32_t n = 0;
    uint64_t flags = spin_lock_irqsave(&g_evt_lock);
    while (n < XHCI_EVT_RING_TRBS && xhci_evt_dequeue(d)) n++;
    /* **置き場も一緒に空にする。**残しておくと、作り直したリングの
     * イベントに古いものが混ざる。**ロックの中で呼ぶ**ので
     * xhci_evt_stash_clear は自分では取らない */
    xhci_evt_stash_clear();
    spin_unlock_irqrestore(&g_evt_lock, flags);
    return n;
}

/* ---- イベントの置き場 ------------------------------------------------------
 *
 * **イベントリングは 1 本しかない。**コマンド完了も、全スロット・全
 * エンドポイントの転送完了も、同じ列に順不同で並ぶ。
 *
 * 待っている相手と違うイベントを捨てると、**そのイベントを待っている側は
 * 永久に取り逃す**。いまは起動時の列挙が終わるまでキーボードのポーリングが
 * 始まらないので害が出ていないが、動作中にハブへ制御転送を投げるように
 * なると (B-1)、**キー入力のイベントが消える**。
 * usb_hid_keyboard_poll() は割り込み TRB を積んだまま戻るので、
 * キーのイベントはいつ来るか分からない。
 *
 * そこで、一致しないイベントは捨てずにここへ退避し、待っている側が
 * 後から拾えるようにする。**B-2 (割り込み化) でも同じ土台が要る**。
 *
 * **並行性は未対処。**ここは今のところタイマ割り込み (キーボード) と
 * タスク文脈 (列挙) の両方から触られる。イベントリング自体が元から
 * 同じ状態なので B-1a では悪化させないが、抜き差しの定期確認を
 * 足すときに手当てが要る */
#define XHCI_EVT_STASH_CAP 16

typedef struct { uint32_t d[4]; } xhci_evt_t;
static xhci_evt_t g_evt_stash[XHCI_EVT_STASH_CAP];
static uint32_t g_evt_stash_head;
static uint32_t g_evt_stash_count;
static uint32_t g_evt_stash_dropped;   /* 溢れて捨てた数。測れるようにしておく */
static uint32_t g_evt_stash_peak;
/* **置き場に何か入るたびに増える。**待っている側は、この値が動いたときだけ
 * 置き場を舐め直す。空回りのたびに 16 個を照合すると、待ち時間の意味が
 * 変わってしまう (A-1 の前は 1 回の待ちで 8000000 回まで回っていた) */
static volatile uint32_t g_evt_stash_gen;

/* type: 32 = Transfer Event / 33 = Command Completion Event。
 * slot/ep/trb は 0 なら「問わない」 */
static int xhci_evt_matches(const uint32_t d[4], uint32_t want_type, uint8_t slot_expect,
                            uint8_t ep_expect, uint64_t trb_expect) {
    uint32_t type = (d[3] >> 10) & 0x3F;
    if (type != want_type) return 0;
    if (want_type == 32) {
        uint8_t slot = (uint8_t)((d[3] >> 24) & 0xFF);
        uint8_t ep = (uint8_t)((d[3] >> 16) & 0x1F);
        uint64_t trb_ptr = (((uint64_t)d[1] << 32) | d[0]) & ~0xFULL;
        if (slot_expect && slot != slot_expect) return 0;
        if (ep_expect && ep != ep_expect) return 0;
        if (trb_expect && trb_ptr != (trb_expect & ~0xFULL)) return 0;
    }
    return 1;
}

static uint32_t g_evt_pscd_count;   /* Port Status Change Event の数 */
static uint32_t g_evt_pscd_port;    /* 最後に変化したポート */
/* 定義は下の「PORTSC を書くときのビット」のところ */
static void xhci_ack_port_change(uint8_t port);

static void xhci_evt_stash_push(const uint32_t d[4]) {
    uint32_t idx;
    uint32_t type = (d[3] >> 10) & 0x3F;

    /* **Port Status Change Event (34) を待っている者は居ない。**
     * 置き場に積むと 16 個をこれで埋めてしまい、本当に要る転送完了を
     * 押し出す。数えて落とす (codex 相談 (c)) */
    if (type == 34U) {
        /* **変化ビットを落とす (A-2)。**落とさないと xHC は同じ変化を
         * 上げ続ける。ポーリングのうちは「うるさいだけ」で済んでいたが、
         * **INTx はレベル駆動なので、落とさないと割り込み嵐になる。**
         * ポート番号は dword0 の bits 31:24 */
        g_evt_pscd_count++;
        xhci_ack_port_change((uint8_t)((d[0] >> 24) & 0xFFU));
        return;
    }
    /* 転送完了 (32) とコマンド完了 (33) 以外も待ち手が居ない */
    if (type != 32U && type != 33U) return;

    if (g_evt_stash_count >= XHCI_EVT_STASH_CAP) {
        /* **溢れたら一番古いものを落とす。**新しいほうが役に立つ */
        g_evt_stash_head = (g_evt_stash_head + 1U) % XHCI_EVT_STASH_CAP;
        g_evt_stash_count--;
        g_evt_stash_dropped++;
    }
    idx = (g_evt_stash_head + g_evt_stash_count) % XHCI_EVT_STASH_CAP;
    g_evt_stash[idx].d[0] = d[0];
    g_evt_stash[idx].d[1] = d[1];
    g_evt_stash[idx].d[2] = d[2];
    g_evt_stash[idx].d[3] = d[3];
    g_evt_stash_count++;
    if (g_evt_stash_count > g_evt_stash_peak) g_evt_stash_peak = g_evt_stash_count;
    g_evt_stash_gen++;
}

/* 条件に合うものを 1 つ取り出して詰める。見つからなければ 0。
 *
 * **配列を後ろから詰め替える間に割り込みが押し込むと壊れる (U-2)。**
 * 呼ばれるのはタスク文脈だけだが、押し込む側は CPU 0 の割り込みなので
 * ロックが要る */
static int xhci_evt_stash_take_locked(uint32_t want_type, uint8_t slot_expect,
                                      uint8_t ep_expect, uint64_t trb_expect,
                                      uint32_t out[4]);

static int xhci_evt_stash_take(uint32_t want_type, uint8_t slot_expect, uint8_t ep_expect,
                               uint64_t trb_expect, uint32_t out[4]) {
    int ret;
    uint64_t flags = spin_lock_irqsave(&g_evt_lock);
    ret = xhci_evt_stash_take_locked(want_type, slot_expect, ep_expect, trb_expect, out);
    spin_unlock_irqrestore(&g_evt_lock, flags);
    return ret;
}

static int xhci_evt_stash_take_locked(uint32_t want_type, uint8_t slot_expect,
                                      uint8_t ep_expect, uint64_t trb_expect,
                                      uint32_t out[4]) {
    uint32_t i;
    for (i = 0; i < g_evt_stash_count; i++) {
        uint32_t idx = (g_evt_stash_head + i) % XHCI_EVT_STASH_CAP;
        if (!xhci_evt_matches(g_evt_stash[idx].d, want_type, slot_expect, ep_expect, trb_expect)) {
            continue;
        }
        out[0] = g_evt_stash[idx].d[0];
        out[1] = g_evt_stash[idx].d[1];
        out[2] = g_evt_stash[idx].d[2];
        out[3] = g_evt_stash[idx].d[3];
        /* 取り出した穴を後ろから詰める */
        for (; i + 1U < g_evt_stash_count; i++) {
            uint32_t a = (g_evt_stash_head + i) % XHCI_EVT_STASH_CAP;
            uint32_t b = (g_evt_stash_head + i + 1U) % XHCI_EVT_STASH_CAP;
            g_evt_stash[a] = g_evt_stash[b];
        }
        g_evt_stash_count--;
        return 1;
    }
    return 0;
}

/* **呼び出し側が g_evt_lock を握っていること** (いまの呼び手は
 * xhci_evt_drain だけ) */
static void xhci_evt_stash_clear(void) {
    g_evt_stash_head = 0;
    g_evt_stash_count = 0;
}

/* ---- イベントリングを吸い出す。**唯一の消費者** ---------------------------
 *
 * codex の指摘 (2026-08-20): 「INTE を立てたままハンドラが無いのは中途半端。
 * ハンドラを実装するなら、イベントリングの消費者を 1 か所に決めるべき」。
 *
 * **ハードウェアのリングを触るのはこの関数だけ。**待っている側は置き場
 * (g_evt_stash) しか見ない。呼ばれるのは 2 か所:
 *
 *   - usb_xhci_irq()  割り込み文脈
 *   - 待っている側    割り込みが来なくても進めるための保険
 *
 * **保険は外さない。**割り込みが配線どおりに来なければ、動きは今までと
 * 同じポーリングに戻るだけで、抜き差しは壊れない。
 *
 * 割り込みを止めてから触る。止めないと、置き場の更新中に割り込みが入って
 * 同じ場所を書く */
static uint32_t xhci_evt_pump(void) {
    uint32_t d[4];
    uint32_t n = 0;
    /* **自 CPU の割り込みを閉じるだけでは足りない (U-2)。**
     * 他のコアの pump と取り出しも締め出す。g_evt_lock の注記を参照 */
    uint64_t flags = spin_lock_irqsave(&g_evt_lock);
    /* **上限を置く。**壊れたリングで永久に回らないように */
    while (n < XHCI_EVT_RING_TRBS && xhci_evt_dequeue(d)) {
        xhci_evt_stash_push(d);
        n++;
    }
    spin_unlock_irqrestore(&g_evt_lock, flags);
    return n;
}

/* ---- A-1: 待ちを回数から時間に変え、割り込みに仕事をさせる ----------------
 *
 * A-2 で測って分かったこと (2026-08-22 実機):
 *
 *   空回り 1 回        221ns
 *   旧上限 8000000 回  1769ms     ← **回数で書いてあった。秒数を誰も知らない**
 *   実際に待つ最大     3ms        ← 転送完了。コマンド完了は 0.25ms
 *   上限に達した回数   0 件 (標本 1479 件)
 *
 * **必要なのは 3ms、待つ気でいたのは 1769ms。**上限を時間で書き直す。
 *
 * それとは別に、**割り込みが仕事をしていなかった** (2026-08-21 §8)。
 * 待っている側が毎回 pump を呼ぶので、割り込みが届く頃にはリングが空で、
 * 拾ったイベント数が 33 で止まっていた。待ちの中身を入れ替える:
 *
 *   いままで                    これから
 *   -----------------------     -----------------------------------------
 *   毎回 pump を呼ぶ            **置き場の世代を見るだけ。埋めるのは割り込み**
 *   回数で数える (8000000)      時刻で数える (XHCI_WAIT_MS)
 *
 * **保険は外さない。**割り込みが配線どおりに来ない機械では置き場が永久に
 * 埋まらないので、**XHCI_PUMP_FALLBACK_MS のあいだ何も来なければ自分で
 * 1 回吸う。**そのときの動きは 1ms 刻みのポーリングに落ちるだけで、
 * 抜き差しは壊れない。
 *
 * **CPU は手放していない。**譲る (kernel_yield) 案は見送った —
 * この待ちが走るのは g_usb_busy が立っている区間、つまり**キーボードの
 * ポーリングを止めている最中**で、譲ると入力が止まる長さが読めなくなる。
 * 譲るなら排他の作り直しが先になる */
#define XHCI_WAIT_MS           100U  /* 待ちの上限。実測最大 3ms の 33 倍 */
/* 保険。これだけ何も来なければ自分で吸う。
 *
 * **いまはこれが本体で、割り込みは仕事をしていない。**理由は保険が速い
 * ことではなかった (2026-08-22 実機):
 *
 *   idle タスクは**割り込みを閉じて走っている。**
 *   arch_task_idle_wait_once() の wfi の中でだけ開く
 *   (include/aarch64/task.h)。usb_hotplug_poll() はその窓の外で
 *   呼ばれる (kernel/sched.c) ので、**制御転送のあいだ割り込みは届かない。**
 *
 * 20ms に延ばして確かめた。**それでも割り込みは取りに来ず**、待ちの最大が
 * 素直に 3ms → 20ms に伸びただけだった。割り込みは巡回 1 回につき 1 度
 * (2.0/秒 = ハブの 500ms 巡回そのもの)、wfi に着いたときだけ届いていた。
 *
 * **したがって 1ms に戻す。**取るのが保険である以上、遅くする理由が無い。
 * 割り込みに仕事をさせるには idle の割り込み方針を変えることになるが、
 * 影響は USB だけではない (bottom_half_run も net_poll も同じ窓の外) */
#define XHCI_PUMP_FALLBACK_MS  1U
/* **時計が進まない機械で永久に回らないための止め。**
 * 旧版は回数で縛られていたので必ず終わった。時間で縛ると、USB_NOW_MS() が
 * 止まっていれば期限が来ない。1 回転は 300ns 前後なので、5000 万回転は
 * 15 秒に相当し、**100ms の待ちが正常に終わる限り絶対に届かない。**
 * ここに当たったときは要約の 到達= が増えて 時間が 0ms になる —
 * **時計を疑う印**として読む */
#define XHCI_WAIT_TURN_CAP     50000000U
/* 「上限まで待つ」を表す loops の値。**キーボードの取り込み (loops=2000) と
 * 区別するための印**で、回数としては使わない */
#define XHCI_WAIT_FULL         0xFFFFFFFFU

typedef struct {
    uint64_t t0;
    uint64_t deadline;
    uint64_t next_pump;
    uint32_t gen;
    uint32_t turns;     /* 回った回数。**較正した空回りとは別物** */
} xhci_waiter_t;

/* **イベントを誰が取ったかを数える。**
 *
 * A-1 を入れた最初の実機で `irq=227/37` — 割り込みは 194 回上がったのに
 * 拾ったイベントは 4 件だった。**「割り込みが仕事をしていない」ことは
 * 分かるが、代わりに誰が取っているのかが分からない。**
 *
 * イベントを吸えるのは 3 か所しかないので、全部数えて突き合わせる:
 *
 *   g_xhci_irq_events    割り込み文脈             usb_xhci_irq()
 *   g_selfpump_events    待ちの保険 (1ms ごと)    xhci_waiter_turn()
 *   g_kbdpump_events     キーボードの取り込み     loops=2000 の従来経路
 *
 * **3 つの合計が、実際に起きたイベントの数になるはず。**合わなければ
 * 数え漏らしている経路がもう 1 つあるということ */
static uint32_t g_selfpump_events;
static uint32_t g_kbdpump_events;

static void xhci_waiter_begin(xhci_waiter_t* w) {
    w->t0 = USB_NOW_MS();
    w->deadline = w->t0 + (uint64_t)XHCI_WAIT_MS;
    w->next_pump = w->t0;             /* 最初の 1 回だけは自分で吸う */
    w->gen = g_evt_stash_gen;
    w->turns = 0;
}

/* 待ちの 1 回転。**1 = 置き場が動いた (取り出しを試せ) / 0 = まだ /
 * -1 = 時間切れ** */
static int xhci_waiter_turn(xhci_waiter_t* w) {
    uint64_t now = USB_NOW_MS();
    w->turns++;

    /* 保険。**毎回は呼ばない** — 呼ぶと割り込みの取り分が無くなる */
    if (now >= w->next_pump) {
        g_selfpump_events += xhci_evt_pump();
        w->next_pump = now + (uint64_t)XHCI_PUMP_FALLBACK_MS;
    }

    if (w->gen != g_evt_stash_gen) {
        w->gen = g_evt_stash_gen;
        return 1;
    }
    if (now >= w->deadline) return -1;
    if (w->turns >= XHCI_WAIT_TURN_CAP) return -1;   /* 時計が止まっている */

    /* **A-1b: ここだけ割り込みを開ける。**
     *
     * この待ちは割り込み文脈では走らない。キーボードの取り込みは
     * loops=2000 の従来経路で、waiter を通らない。**開けても入れ子には
     * ならない。**
     *
     * 開けている間に走り得るのは 2 つだけで、どちらも安全:
     *
     *   xHCI    usb_xhci_irq() -> xhci_evt_pump()。**これが狙い**
     *   タイマ  aarch64_kbd_tick() は g_usb_busy で即戻る。
     *           task_on_timer_tick() は resched_pending を立てるだけで、
     *           切り替えは idle ループの底でしか起きない */
    {
        uint64_t tok = usb_arch_irq_window_begin();
        USB_CPU_RELAX();
        usb_arch_irq_window_end(tok);
    }
    return 0;
}

/* ---- A-2: 空回りの実時間を測る -------------------------------------------
 *
 * **A-1 の根拠。**待ちを時間で決めるには、まず旧上限 8000000 という
 * **回数**が何秒なのかを知る必要があった。測ったのは 3 つ:
 *
 *   ① 空回り 1 回の実時間      → 旧上限まで回ると何 ms になるか (較正)
 *   ② 完了まで実際に待った時間 → 新しい上限を決める根拠
 *   ③ 上限に達したときの実時間 → ①の答え合わせ。起きなければ 0 件のまま
 *
 * **A-1 を入れた後も較正は残す。**待ちの中身が変わっても、この機械の
 * 空回り 1 回が何 ns なのかは変わらず、回帰の目安になる
 *
 * ① は**回数を決めて時間を測るのではなく、時間を決めて回数を数える。**
 * USB_NOW_MS() は 1ms 刻みしかないので、短すぎる測定は 0 か 1 の
 * どちらかになって桁が合わない */
/* **A-1 で外した旧上限。**各所に 8000000 と直書きされていた回数で、
 * いまはどこも使っていない。**較正の記録にだけ残す** — この値が何秒
 * だったのかが A-1 (100ms) の根拠そのものなので、消すと理由が消える */
#define XHCI_OLD_SPIN_LIMIT   8000000U
#define XHCI_CALIB_MS     50U        /* 較正に使う時間 */
#define XHCI_CALIB_CHUNK  256U       /* 時刻を読む間隔 (回) */

static uint32_t g_spin_loops_per_ms;   /* 0 なら未較正 */
static uint32_t g_spin_calib_loops;
static uint32_t g_spin_calib_ms;

/* ② 待ちの実測。転送完了 (32) とコマンド完了 (33) は桁が違うので分けて持つ */
typedef struct {
    uint32_t calls;        /* 待った回数 */
    uint32_t immediate;    /* 置き場に既にあって 1 回も回らずに済んだ回数 */
    uint32_t max_loops;    /* 回った回数の最大 */
    uint32_t max_ms;       /* 経過 ms の最大 (loops からの換算の答え合わせ) */
    uint32_t timeouts;     /* ③ 上限まで回って諦めた回数 */
    uint32_t timeout_ms;   /* そのときの実時間 */
} xhci_wait_stat_t;

static xhci_wait_stat_t g_wait_xfer;   /* 転送完了待ち */
static xhci_wait_stat_t g_wait_cmd;    /* コマンド完了待ち */

static void xhci_wait_note(xhci_wait_stat_t* s, uint32_t loops, uint64_t t0) {
    uint32_t ms = (uint32_t)(USB_NOW_MS() - t0);
    s->calls++;
    if (loops == 0) s->immediate++;
    if (loops > s->max_loops) s->max_loops = loops;
    if (ms > s->max_ms) s->max_ms = ms;
}

static void xhci_wait_note_timeout(xhci_wait_stat_t* s, uint64_t t0) {
    s->calls++;
    s->timeouts++;
    s->timeout_ms = (uint32_t)(USB_NOW_MS() - t0);
}

/* **待ちループの本体と同じことを一定時間くり返して、回数を数える。**
 *
 * 時刻を毎回読んではいけない。USB_NOW_MS() は cntpct の読み出しと 64bit の
 * 除算で、**測ろうとしている pump より重い可能性がある。**それを混ぜると
 * 測ったものが別物になるので、XHCI_CALIB_CHUNK 回ごとにだけ読む */
static void xhci_spin_calibrate(void) {
    uint64_t t0, elapsed;
    uint32_t n = 0;
    uint32_t gen = g_evt_stash_gen;

    /* **刻みの境目から始める。**t0 を読んだ瞬間が 1ms の途中だと、
     * 最初の 1ms が実際より短く数えられる */
    t0 = USB_NOW_MS();
    while (USB_NOW_MS() == t0) USB_CPU_RELAX();
    t0 = USB_NOW_MS();

    while ((USB_NOW_MS() - t0) < (uint64_t)XHCI_CALIB_MS) {
        uint32_t k;
        for (k = 0; k < XHCI_CALIB_CHUNK; k++) {
            /* xhci_poll_transfer_event の空回り 1 回とそろえる */
            (void)xhci_evt_pump();
            if (g_evt_stash_gen != gen) gen = g_evt_stash_gen;
        }
        n += XHCI_CALIB_CHUNK;
    }
    elapsed = USB_NOW_MS() - t0;
    if (elapsed == 0) elapsed = 1;

    g_spin_calib_loops = n;
    g_spin_calib_ms = (uint32_t)elapsed;
    g_spin_loops_per_ms = (uint32_t)((uint64_t)n / elapsed);

    puts("[usb] A-2 calibration: ");
    putdec(g_spin_calib_ms);
    puts("ms for ");
    putdec(g_spin_calib_loops);
    puts(" times -> 1 time = ");
    putdec(g_spin_loops_per_ms ? (1000000ULL / (uint64_t)g_spin_loops_per_ms) : 0ULL);
    puts("ns, old limit ");
    putdec(XHCI_OLD_SPIN_LIMIT);
    puts(" times = ");
    putdec(g_spin_loops_per_ms ? ((uint64_t)XHCI_OLD_SPIN_LIMIT / (uint64_t)g_spin_loops_per_ms) : 0ULL);
    puts("ms equivalent -> current limit is ");
    putdec(XHCI_WAIT_MS);
    puts("ms\r\n");
}

static uint32_t g_xhci_irq_count;      /* 割り込みが実際に来た回数 */
static uint32_t g_xhci_irq_events;     /* 割り込みで拾ったイベントの数 */
static uint32_t g_irq_eint;            /* 入口で EINT が立っていた回数 */
static uint32_t g_irq_noeint;          /* 立っていなかった回数 (よそのもの) */
static uint32_t g_irq_pcd;             /* 入口で PCD が立っていた回数 */

/* **口の開け方を知らないアーキでは何もしない。**動きはポーリングのまま。
 * aarch64 は kernel/aarch64/boot.c の強い定義が勝つ */
__attribute__((weak)) void usb_arch_irq_enable(void) {}

/* ---- A-1b: 待っているあいだだけ割り込みを開ける --------------------------
 *
 * **idle タスクは割り込みを閉じて走っている。**開くのは
 * arch_task_idle_wait_once() の wfi の中だけで (include/aarch64/task.h)、
 * usb_hotplug_poll() はその窓の外で呼ばれる (kernel/sched.c)。
 * したがって**制御転送のあいだ xHCI の割り込みは届かない。**
 *
 * 2026-08-22 の実機で確定した。イベントを吸える 3 か所を全部数えると:
 *
 *   irq=37  保険=33788  kbd=0     (82 分)
 *
 * 割り込みは 82 分で 9889 回上がっているのに、拾ったのは起動時の 37 件で
 * 止まっている。保険を 1ms → 20ms に延ばしても取りに来ず、待ちの最大が
 * 素直に 3ms → 20ms に伸びただけだった。**閉じているのだから届きようがない。**
 *
 * **開ける範囲は待ちの 1 回転ぶんだけ。**
 *
 * 待ち全体を開けっぱなしにする案は採らなかった。**出口が増えるほど
 * 閉じ忘れが漏れる** — 待ちは転送側とコマンド側で 4 か所あり、それぞれ
 * 複数の return を持つ。1 回転で閉じる形なら漏れようがない。
 * 回転は 1ms に数千回あるので、上がっている割り込みは即座に取られる。
 *
 * **既定は何もしない。**実装しないアーキでは動きがいままでのポーリングの
 * ままになるだけで、抜き差しは壊れない */
__attribute__((weak)) uint64_t usb_arch_irq_window_begin(void) { return 0; }
__attribute__((weak)) void usb_arch_irq_window_end(uint64_t token) { (void)token; }

/* **xHCI の割り込み入口 (A-1)。**アーキ側の IRQ ハンドラから呼ばれる。
 *
 * **発生源を先に落とす。**INTx はレベル駆動なので、落とさないまま戻ると
 * 同じ割り込みが上がり続けて何も進まなくなる。落とすのは 2 つ:
 *
 *   USBSTS.EINT (bit 3)      RW1C
 *   IMAN.IP     (bit 0)      RW1C
 *
 * 落としてから吸い出す。順番を逆にすると、吸い出しの最中に来たイベントの
 * ぶんの表明を消してしまう */
void usb_xhci_irq(void) {
    uint32_t n;
    g_xhci_irq_count++;
    if (!g_op_regs || !g_rt_regs) return;

    {
        uint32_t sts = mmio_read32(g_op_regs, 0x04);

        /* **入口で何が立っていたかを数える。**
         * 20ms 保険の実機 (2026-08-22) で「イベントが 20ms リングに座って
         * いるあいだに割り込みが 220 回上がったのに 4 件しか拾えない」と
         * いう結果が出た。**上がっている割り込みが本当に自分のものなのか**
         * を、推測ではなく数で分ける */
        if (sts & (1U << 3)) g_irq_eint++; else g_irq_noeint++;
        if (sts & (1U << 4)) g_irq_pcd++;
        /* **ここから UART に出さないこと。**115200 で 1 行 2.6ms かかり、
         * 割り込み文脈でそれだけ止まる。実際、生の usbsts を 8 行出す
         * 探針を置いたら**待ちの最大が 3ms から 13ms に化けた** —
         * 計器が測定対象を壊していた (2026-08-22)。
         * 数えるだけにして、出すのは 60 秒ごとの要約側に任せる */

        /* **止まっていない発生源を全部落とす。**
         *
         *   EINT (bit 3)  RW1C   イベントが載った
         *   PCD  (bit 4)  RW1C   ポートに変化があった
         *
         * **PCD を落としていなかった。**初期化で 1 回消したきりで、
         * ハンドラは EINT しか触っていない。INTx はレベル駆動なので、
         * 落とさない発生源が 1 つでもあると線が下がらない。
         * ポートの変化そのものは Port Status Change Event として
         * リングにも載り、xhci_evt_stash_push が PORTSC の変化ビットを
         * 落としている (A-2)。**PCD はその要約ビットなので、
         * ここで落としても取りこぼしにはならない** */
        {
            uint32_t clear = sts & ((1U << 3) | (1U << 4));
            if (clear) mmio_write32(g_op_regs, 0x04, clear);
        }
    }
    {
        uint32_t iman = mmio_read32(g_rt_regs, 0x20 + 0x00);
        if (iman & 1U) mmio_write32(g_rt_regs, 0x20 + 0x00, iman | 1U);
    }

    n = xhci_evt_pump();
    g_xhci_irq_events += n;
}

static int xhci_poll_cmd_completion(uint64_t* out_cmd_ptr, uint8_t* out_cc, uint8_t* out_slot_id, uint32_t loops) {
    uint32_t d[4];
    if (!g_rt_regs || !out_cmd_ptr || !out_cc || !out_slot_id) return -1;

    /* **置き場を見る。**先に来てしまった完了がここに居ることがある。
     * 割り込みが拾ったものもここに入っている。
     *
     * **A-1: 呼ぶ側は loops=0 で入ってくる。**待ちの回転は
     * xhci_waiter_turn が持っていて、ここは置き場を見るだけ */
    if (xhci_evt_stash_take(33, 0, 0, 0, d)) goto got;

    {
        uint32_t gen = g_evt_stash_gen;
        for (uint32_t i = 0; i < loops; i++) {
            /* **保険。**割り込みが来ていれば置き場は既に埋まっている */
            (void)xhci_evt_pump();
            if (g_evt_stash_gen == gen) continue;   /* 何も増えていない */
            gen = g_evt_stash_gen;
            if (xhci_evt_stash_take(33, 0, 0, 0, d)) goto got;

            /* **転送完了だけが来た** — 呼ぶ側は 1 を
             * 「まだコマンドの番ではない」と読んで回り続ける */
            *out_cmd_ptr = 0;
            *out_cc = 0xFF;
            *out_slot_id = 0;
            return 1;
        }
    }

    return -1;

got:
    *out_cmd_ptr = ((uint64_t)d[1] << 32) | d[0];
    *out_cc = (uint8_t)((d[2] >> 24) & 0xFF);
    *out_slot_id = (uint8_t)((d[3] >> 24) & 0xFF);
    return 0;
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
        puts("[usb] drained ");
        putdec(stale);
        puts(" discarded\r\n");
    }

    uint64_t cmd_ptr = xhci_cmd_submit(0, 0, 0, (23U << 10)); // No-Op Command TRB
    xhci_waiter_t w;
    if (!cmd_ptr) return -1;

    xhci_waiter_begin(&w);
    for (;;) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 0);
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
        if (xhci_waiter_turn(&w) < 0) break;
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
                putdec(i);
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
            if (!shown) puts("[usb] evt[0..15] all zero (none written)\r\n");
        }
    }
    return -3;
}

static int xhci_cmd_enable_slot(uint8_t* out_slot_id) {
    if (out_slot_id) *out_slot_id = 0;
    uint64_t cmd_ptr = xhci_cmd_submit(0, 0, 0, (9U << 10)); // Enable Slot Command
    xhci_waiter_t w;
    if (!cmd_ptr) return -1;

    xhci_waiter_begin(&w);
    for (;;) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 0);
        if (r == 0 && (ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL)) {
            xhci_wait_note(&g_wait_cmd, w.turns, w.t0);   /* A-2 */
            if (cc != 1) return -2;
            if (out_slot_id) *out_slot_id = slot;
            return (slot != 0) ? 0 : -3;
        }
        if (xhci_waiter_turn(&w) < 0) break;
    }
    xhci_wait_note_timeout(&g_wait_cmd, w.t0);   /* A-2 */
    return -4;
}

static int xhci_wait_cmd_completion(uint64_t cmd_ptr, uint8_t slot_expect, uint8_t* out_cc, uint8_t* out_slot) {
    xhci_waiter_t w;
    xhci_waiter_begin(&w);
    for (;;) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 0);
        if (r == 0 && (ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL)) {
            xhci_wait_note(&g_wait_cmd, w.turns, w.t0);   /* A-2 */
            if (out_cc) *out_cc = cc;
            if (out_slot) *out_slot = slot;
            if (slot_expect && slot != slot_expect) return -2;
            return 0;
        }
        if (xhci_waiter_turn(&w) < 0) break;
    }
    xhci_wait_note_timeout(&g_wait_cmd, w.t0);   /* A-2 */
    return -1;
}

/* Disable Slot。**抜かれたスロットを返す。**返さないと抜き差しを
 * 繰り返すうちにスロットを使い切る (この xHC は 32 本) */
static int xhci_cmd_disable_slot(uint8_t slot_id) {
    uint8_t cc = 0xFF, slot = 0;
    uint64_t cmd_ptr;
    if (slot_id == 0) return -1;
    cmd_ptr = xhci_cmd_submit(0, 0, 0, (10U << 10) | ((uint32_t)slot_id << 24));
    if (!cmd_ptr) return -2;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) != 0) return -3;
    return (cc == 1) ? 0 : -4;
}

/* **イベントが指す TRB の番地も返す。**
 * これが無いと Setup / Data / Status のどの段で落ちたかが分からない
 * (2026-08-18 codex 相談) */
static uint64_t g_last_evt_trb = 0;
static uint8_t  g_last_cc = 0;
static uint32_t g_last_residual = 0;

static int xhci_poll_transfer_event(uint8_t slot_expect, uint8_t ep_expect, uint64_t trb_expect,
                                    uint32_t loops, uint8_t* out_cc, uint32_t* out_residual) {
    uint32_t d[4];
    if (!g_rt_regs) return -1;

    /* **先に置き場を見る。**他のエンドポイントを待っている間に来た
     * ぶんがここに溜まっている。割り込みが拾ったものもここに入る */
    if (loops == XHCI_WAIT_FULL) {
        /* ---- A-1: 時間で待つ。埋めるのは割り込み ---------------------- */
        xhci_waiter_t w;
        xhci_waiter_begin(&w);
        if (xhci_evt_stash_take(32, slot_expect, ep_expect, trb_expect, d)) {
            xhci_wait_note(&g_wait_xfer, 0, w.t0);   /* 1 回も回らずに済んだ */
            goto got;
        }
        for (;;) {
            int r = xhci_waiter_turn(&w);
            if (r < 0) {
                xhci_wait_note_timeout(&g_wait_xfer, w.t0);
                return -1;
            }
            /* 待っている相手でなければ置き場に残る。捨てない */
            if (r > 0 && xhci_evt_stash_take(32, slot_expect, ep_expect, trb_expect, d)) {
                xhci_wait_note(&g_wait_xfer, w.turns, w.t0);
                goto got;
            }
        }
    }

    /* ---- キーボードの取り込み (loops=2000) -----------------------------
     *
     * **ここはタイマ割り込みの中で走る。**戻り値 -1 は「まだ来ていない」で、
     * 待ちではない。時間で測る意味も、譲る余地も無いので従来のまま */
    if (xhci_evt_stash_take(32, slot_expect, ep_expect, trb_expect, d)) goto got;

    {
        uint32_t gen = g_evt_stash_gen;
        for (uint32_t i = 0; i < loops; i++) {
            g_kbdpump_events += xhci_evt_pump();
            if (g_evt_stash_gen == gen) continue;   /* 何も増えていない */
            gen = g_evt_stash_gen;
            if (xhci_evt_stash_take(32, slot_expect, ep_expect, trb_expect, d)) goto got;
        }
    }
    return -1;

got:
    {
        uint64_t trb_ptr = (((uint64_t)d[1] << 32) | d[0]) & ~0xFULL;
        if (out_cc) *out_cc = (uint8_t)((d[2] >> 24) & 0xFF);
        /* **cc=27 (Stopped - Length Invalid) のときこの値は無効。**
         * 読む側が判断できるよう、ここでは素通しする (E-3 の注記を見ること) */
        if (out_residual) *out_residual = (d[2] & 0x00FFFFFFU);
        g_last_evt_trb = trb_ptr;
        g_last_cc = (uint8_t)((d[2] >> 24) & 0xFF);
        g_last_residual = (d[2] & 0x00FFFFFFU);
    }
    return 0;
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
    g_ep0_slot[slot_id].ring_phys = (uint64_t)ep0_ring;
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
    ep[255 * 4 + 0] = (uint32_t)(g_ep0_slot[slot_id].ring_phys & 0xFFFFFFFFU);
    ep[255 * 4 + 1] = (uint32_t)(g_ep0_slot[slot_id].ring_phys >> 32);
    ep[255 * 4 + 2] = 0;
    ep[255 * 4 + 3] = (6U << 10) | (1U << 1) | 1U;
    g_ep0_slot[slot_id].enq_idx = 0;
    g_ep0_slot[slot_id].cycle = 1;

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
    ic[ep0_off + 2] = (uint32_t)((g_ep0_slot[slot_id].ring_phys & ~0xFULL) | 1U); // TR Dequeue + DCS
    ic[ep0_off + 3] = (uint32_t)(g_ep0_slot[slot_id].ring_phys >> 32);
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
        putdec(cc);
        puts("\r\n");
        return -6;
    }
    return 0;
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

/* ---- TRB を 1 個ずつ積む --------------------------------------------------
 *
 * **書いた枠の index を返す。**TD が Link をまたぐと、Setup の次が
 * idx+1 とは限らなくなる。落ちたときにどの段かを言うには、3 つの実 index を
 * 覚えておくしかない。
 *
 * ## なぜ「予約して飛ぶ」のをやめたか
 *
 * 2026-08-20 に入れた xhci_ring_reserve() は、n 個が枠 255 に届くなら
 * **積む前に Link を越えて idx を 0 に戻す**作りだった。TD が Link を
 * またがなくなる代わりに、**[idx, 254] が書かれないまま残る**。
 *
 * 2026-08-21 の実機でそこが壊れていることを実測した。
 *
 *   [ring] ... idx=254 n=3 隙間=1 cycle 1 -> 0
 *   [snap] tail[253] type=4 cy=1     直前の TD。xHC は CCS=1 で消費
 *   [snap] tail[254] type=0 cy=0   ★ 隙間。cy=0 だが xHC の CCS は 1
 *   [snap] tail[255] type=6 cy=1     Link は正しい。**ここまで届かない**
 *
 * **xHC は 254 で止まる。**サイクルが合わないので「まだ積まれていない」と
 * 読み、Link に到達しない。usbsts は健全、epstate も 1 (Running)、
 * イベントリングも空 — **ただ待っているだけ**の状態になる。
 *
 * ## 直し方 (MikanOS の Ring::Push と同じ)
 *
 * **1 個ずつ書き、枠 255 に来たらその場で Link を書いてサイクルを反転する。**
 * 毎枠を順に埋めるので隙間が生じない。**TD が Link をまたぐのは許す** —
 * xHC は Link を辿って先頭に戻り、TC=1 で自分の CCS も反転するので、
 * またいだ先の TRB とも一致する。
 *
 * Link は**「今の」サイクルで書いてから**反転する。順番を逆にすると
 * xHC から見て Link が「まだ積まれていない」ことになる */
/* リングが一周した回数。**印字は 4 回で止めるが、数は取り続ける** (A-3) */
static uint32_t g_ring_wrap_count;

static uint32_t xhci_ring_push(uint64_t ring_phys, uint32_t* idx, uint32_t* cycle,
                               uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile uint32_t* ring = (volatile uint32_t*)USB_VIRT((void*)ring_phys);
    uint32_t at = *idx;

    ring[at * 4 + 0] = d0;
    ring[at * 4 + 1] = d1;
    ring[at * 4 + 2] = d2;
    /* **サイクルビットは呼び出し側から渡させない。**d3 の bit0 は必ずここで
     * 入れる。渡す側が忘れると、その TRB は永久に消費されない */
    ring[at * 4 + 3] = (d3 & ~1U) | (*cycle & 1U);

    (*idx)++;
    if (*idx == 255U) {
        ring[255 * 4 + 0] = (uint32_t)(ring_phys & 0xFFFFFFFFU);
        ring[255 * 4 + 1] = (uint32_t)(ring_phys >> 32);
        ring[255 * 4 + 2] = 0;
        ring[255 * 4 + 3] = (6U << 10) | (1U << 1) | (*cycle & 1U); /* Link, TC=1 */
        USB_MB();   /* xHC に見せてから自分のサイクルを反転する */

        g_ring_wrap_count++;
        /* **最初の 4 回だけ出す。**次に終端がらみを疑うとき、まず要るのは
         * 「一周は起きているのか」で、それは数行あれば足りる。
         * 抜き差しのポーリングは 10 秒で一周するので、出しっぱなしにすると
         * 長く動かしたときにログが埋まる (2026-08-21 実機で 80 周) */
        {
            static uint32_t told;
            if (told < 4U) {
                told++;
                usb_arch_console_begin();
                puts("[ring] wrote Link at slot 255 ring=0x");
                puthex(ring_phys);
                puts(" cycle ");
                putdec(*cycle);
                puts(" -> ");
                putdec((*cycle) ^ 1U);
                puts(told == 4U ? "  (suppressing further output)\r\n" : "\r\n");
                usb_arch_console_end();
            }
        }

        *idx = 0;
        *cycle ^= 1U;
    }
    return at;
}

static int xhci_ep0_get_device_descriptor(uint8_t slot_id) {
    if (slot_id == 0 || g_ep0_slot[slot_id].ring_phys == 0 || g_ep0_buf_phys == 0) return -1;

    uint64_t base = g_ep0_slot[slot_id].ring_phys;
    uint32_t* pidx = &g_ep0_slot[slot_id].enq_idx;
    uint32_t* pcy  = &g_ep0_slot[slot_id].cycle;

    // USB Device Descriptor request (18 bytes):
    // bmRequestType=0x80, bRequest=GET_DESCRIPTOR(6), wValue=0x0100, wIndex=0, wLength=18
    uint64_t setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0100 << 16;
    setup |= (uint64_t)0 << 32;
    setup |= (uint64_t)18 << 48;

    /* **1 個ずつ積む。**枠 255 に来たら push がその場で Link を書いて
     * サイクルを反転する。**TD が Link をまたぐのは許す** */
    (void)xhci_ring_push(base, pidx, pcy,                      /* Setup: IDT, TRT=IN */
                         (uint32_t)(setup & 0xFFFFFFFFU),
                         (uint32_t)(setup >> 32), 8,
                         (2U << 10) | (1U << 6) | (3U << 16));
    (void)xhci_ring_push(base, pidx, pcy,                      /* Data: DIR=IN */
                         (uint32_t)(g_ep0_buf_phys & 0xFFFFFFFFU),
                         (uint32_t)(g_ep0_buf_phys >> 32), 18,
                         (3U << 10) | (1U << 16));
    (void)xhci_ring_push(base, pidx, pcy, 0, 0, 0,             /* Status + IOC */
                         (4U << 10) | (1U << 5));

    USB_MB(); /* TRB を書き終えてからドアベル */
    // Ring EP0 doorbell (DB[slot], target endpoint 1)
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);

    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, XHCI_WAIT_FULL, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        /* **落ちた理由を出す。**cc が無いと推測しかできない。
         * 3 = Babble (相手のパケットがこちらの MPS より長い)
         * 4 = USB Transaction Error / 6 = Stall Error
         * 26/27/28 は Stop Endpoint 系 — 下の注記を見ること */
        puts("[usb] device desc cc=");
        putdec(cc);
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
                puts("[usb] device desc is ");
                putdec(i + 1);
                puts(" attempt(s), passed\r\n");
            }
            return 0;
        }
        (void)xhci_ep0_recover_stall(slot_id);
        usb_delay_ms(50);
    }
    return rc;
}

/* 失敗したときに出力デバイス文脈を出す (定義は下) */
static void xhci_dump_dev_ctx(const char* tag, uint64_t out_ctx_phys);

/* **制御転送の 3 段は、それぞれ別の TD にする。**
 *
 * リング上に 3 つ続けて置き、ドアベルを 1 回叩くのは構わないが、
 * **Chain ビットで繋いで 1 つの TD にしてはいけない** (xHCI の
 * TD Usage Rules 違反)。Setup TRB は IDT=1 なので、同じ TD に他の
 * Transfer TRB を含めること自体が違反になる。
 *
 * 以前ここは Setup / Data に CH を立てていた。**MPS=8 では通っていたが、
 * MPS を 16 以上にすると Status 段で cc=6 (Stall Error) になり、EP0 が
 * Halted に落ちた** (実機 2026-08-19 の測定)。壊れた 1-TD 構成では
 * short packet の処理が次に進む先を誤る (2026-08-19 codex 相談) */
static uint32_t g_ep0_seq = 0;

/* 定義は下の「止まった瞬間を丸ごと写す」 */
static int g_snap_done = 0;
static void xhci_freeze_snapshot(const char* tag, uint8_t slot_id, uint32_t idx0);

/* **要求 1 つずつに連番を振って出す。**どこまで通ってどこで止まったかを
 * 後から並べ直せないと、失敗位置の照合ができない (codex 相談 (e)-2)。
 * 最初の 60 件だけ。以降は静かにする */
static void ep0_trace(uint8_t slot_id, const char* kind, uint64_t setup, uint32_t idx0) {
    g_ep0_seq++;
    if (g_ep0_seq > 60U) return;
    /* **1 行を 14 回に分けて組み立てている (M-2)。**囲まないと、途中に
     * 別のコアの行が挟まる。実機で emmc2 の part 行と混ざった */
    usb_arch_console_begin();
    puts("[ep0] seq="); putdec(g_ep0_seq);
    puts(" slot="); putdec(slot_id);
    puts(" "); puts(kind);
    puts(" bReq=0x"); puthex_n((uint32_t)((setup >> 8) & 0xFFU), 2);
    puts(" wIndex="); putdec((setup >> 32) & 0xFFFFU);
    puts(" idx0="); putdec(idx0);
    puts(" cy="); putdec(g_ep0_slot[slot_id].cycle);
    puts("\r\n");
    usb_arch_console_end();
}

static int xhci_ep0_control_in(uint8_t slot_id, uint64_t setup, uint64_t data_phys, uint32_t len) {
    if (slot_id == 0 || g_ep0_slot[slot_id].ring_phys == 0 || data_phys == 0 || len == 0 || len > 4096) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)USB_VIRT((void*)g_ep0_slot[slot_id].ring_phys);
    uint64_t base = g_ep0_slot[slot_id].ring_phys;
    uint32_t* pidx = &g_ep0_slot[slot_id].enq_idx;
    uint32_t* pcy  = &g_ep0_slot[slot_id].cycle;
    uint32_t i_setup, i_data, i_status;

    /* **1 個ずつ積む。**枠 255 に来たら push がその場で Link を書く。
     * **書いた枠を覚えておく** — TD が Link をまたぐと i_setup+1 が
     * Data とは限らないので、落ちたときにどの段かを言えなくなる */
    i_setup  = xhci_ring_push(base, pidx, pcy,                 /* Setup: IDT, TRT=IN */
                              (uint32_t)(setup & 0xFFFFFFFFU),
                              (uint32_t)(setup >> 32), 8,
                              (2U << 10) | (1U << 6) | (3U << 16));
    i_data   = xhci_ring_push(base, pidx, pcy,                 /* Data: DIR=IN */
                              (uint32_t)(data_phys & 0xFFFFFFFFU),
                              (uint32_t)(data_phys >> 32), len,
                              (3U << 10) | (1U << 16));
    i_status = xhci_ring_push(base, pidx, pcy, 0, 0, 0,        /* Status + IOC */
                              (4U << 10) | (1U << 5));

    ep0_trace(slot_id, "IN ", setup, i_setup);
    USB_MB(); /* TRB を書き終えてからドアベル */
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, XHCI_WAIT_FULL, &cc, &residual) < 0) {
        /* **イベントが 1 つも来ないまま待ち切れた。**
         * ここは長いあいだ無言だった。CC が付く失敗と違って
         * 「そもそも返ってこない」ことが見えないと切り分けられない */
        puts("[usb] ep0 IN *** no event arrived slot=");
        putdec(slot_id);
        puts(" seq=");
        putdec(g_ep0_seq);
        puts(" trb=");
        putdec(i_setup);
        puts("/");
        putdec(i_data);
        puts("/");
        putdec(i_status);        /* **またいでいれば番号が飛ぶ。**それが見える */
        puts(" enq=");
        putdec(g_ep0_slot[slot_id].enq_idx);
        puts(" cy=");
        putdec(g_ep0_slot[slot_id].cycle);
        puts("\r\n");
        /* **最初の 1 回だけ、直す前に全部写す** */
        if (!g_snap_done) {
            g_snap_done = 1;
            xhci_freeze_snapshot("ep0 IN timeout", slot_id, i_setup);
        }
        return -2;
    }
    if (cc != 1) {
        /* **落ちたときに要るものを 1 か所で全部出す。**
         * どの段で落ちたか (イベントが指す TRB の番地から)、積んだ 3 つの
         * dword3 (CH が落ちているか)、EP0 の状態と取り出し位置。
         * これが無いと「STALL した」以上のことが言えない */
        puts("[usb] ep0 IN *** cc=");
        putdec(cc);
        puts(" resid=");
        putdec(residual);
        puts(" len=");
        putdec(len);
        puts(" mps=");
        putdec(g_usb_ep0_mps);
        puts(" setup=0x");
        puthex(setup);
        puts("\r\n[usb]   stage=");
        /* **積んだ枠から引く。**TD が Link をまたぐと連番ではなくなるので、
         * idx0+1 / idx0+2 で当てにいくと段を取り違える */
        if (g_last_evt_trb == base + (uint64_t)i_setup * 16U)       puts("Setup");
        else if (g_last_evt_trb == base + (uint64_t)i_data * 16U)   puts("Data");
        else if (g_last_evt_trb == base + (uint64_t)i_status * 16U) puts("Status");
        else                                                        puts("unknown");
        puts(" evt=0x");
        puthex(g_last_evt_trb);
        puts(" dw3=0x");
        puthex(ep[i_setup * 4 + 3]); puts(" 0x");
        puthex(ep[i_data * 4 + 3]); puts(" 0x");
        puthex(ep[i_status * 4 + 3]);
        puts("\r\n");
        xhci_dump_dev_ctx("failed", g_output_ctx_phys);
        return -3;
    }
    return 0;
}

static int xhci_ep0_control_no_data(uint8_t slot_id, uint64_t setup) {
    if (slot_id == 0 || g_ep0_slot[slot_id].ring_phys == 0) return -1;

    uint64_t base = g_ep0_slot[slot_id].ring_phys;
    uint32_t* pidx = &g_ep0_slot[slot_id].enq_idx;
    uint32_t* pcy  = &g_ep0_slot[slot_id].cycle;

    /* Setup / Status の 2 つ (データ無し) を 1 個ずつ積む */
    (void)xhci_ring_push(base, pidx, pcy,                  /* Setup: IDT, TRT=No Data */
                         (uint32_t)(setup & 0xFFFFFFFFU),
                         (uint32_t)(setup >> 32), 8,
                         (2U << 10) | (1U << 6));
    (void)xhci_ring_push(base, pidx, pcy, 0, 0, 0,         /* Status IN + IOC */
                         (4U << 10) | (1U << 5) | (1U << 16));

    USB_MB(); /* TRB を書き終えてからドアベル */
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, XHCI_WAIT_FULL, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] ep0 no-data cc=");
        putdec(cc);
        puts(" residual=");
        putdec(residual);
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
/* **変化ビットは上位 16 ビット側。**GET_STATUS は wPortStatus と
 * wPortChange を 2 語続けて返し、こちらは 32 ビットに詰めて持っている。
 * 起動時の実測でも 0x00010101 = 接続あり + 電源 + **接続が変化した** */
#define HUB_C_PORT_CONNECTION (1U << 16)

static uint8_t g_hub_slot_id = 0;
static uint8_t g_hub_nbr_ports = 0;

/* ---- 出力デバイス文脈をそのまま出す ----------------------------------------
 *
 * **xHC が実際に持っている値**を見ないと、こちらが書いたつもりの値と
 * 突き合わせられない。入力文脈を出しても意味が無い (2026-08-18 codex 相談)。
 *
 * 出力文脈は index 0 = Slot Context / index 1 = EP0 Context。
 * 入力文脈と違って先頭に Input Control Context が無い */
/* **スロット番号から出力デバイス文脈を引く。**
 *
 * g_output_ctx_phys は「最後に Address Device したデバイス」のもので、
 * ハブ (slot 1) を見たいときにキーボード (slot 2) の文脈を出してしまう。
 * DCBAA を引けば正しいものが取れる (codex 相談 2026-08-20 (e)-1) */
static uint64_t xhci_dev_ctx_of(uint8_t slot_id) {
    volatile uint64_t* dcbaa;
    if (g_dcbaap_phys == 0 || slot_id == 0) return 0;
    dcbaa = (volatile uint64_t*)USB_VIRT((void*)g_dcbaap_phys);
    return dcbaa[slot_id];
}

/* EP0 の EP State を取る。0=Disabled 1=Running 2=Halted 3=Stopped 4=Error */
static uint32_t xhci_ep0_state(uint8_t slot_id) {
    uint64_t ctx = xhci_dev_ctx_of(slot_id);
    volatile uint32_t* oc;
    if (!ctx) return 0xFFU;
    oc = (volatile uint32_t*)USB_VIRT((void*)ctx);
    return oc[(g_xhci_ctx_size / 4U) + 0] & 7U;
}

static void xhci_dump_dev_ctx(const char* tag, uint64_t out_ctx_phys) {
    volatile uint32_t* oc;
    uint32_t stride_dw = g_xhci_ctx_size / 4U;
    uint32_t s0, s1, s2, s3, e0, e1, e2, e3, e4;

    if (out_ctx_phys == 0) {
        puts("[ctx] ");
        puts(tag);
        puts(" no context\r\n");
        return;
    }
    oc = (volatile uint32_t*)USB_VIRT((void*)out_ctx_phys);
    s0 = oc[0]; s1 = oc[1]; s2 = oc[2]; s3 = oc[3];
    e0 = oc[stride_dw + 0]; e1 = oc[stride_dw + 1]; e4 = oc[stride_dw + 4];
    /* **TR Dequeue Pointer も見る。**Evaluate Context が取り出し位置に
     * 触っていないことは、ここを出さないと言えない (2026-08-19) */
    e2 = oc[stride_dw + 2]; e3 = oc[stride_dw + 3];

    puts("[ctx] ");
    puts(tag);
    puts(" slot dw=0x");
    puthex(s0); puts(" 0x"); puthex(s1); puts(" 0x"); puthex(s2); puts(" 0x"); puthex(s3);
    puts("\r\n[ctx]   route=0x"); puthex_n(s0 & 0xFFFFFU, 5);
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
    puts(" ep0 dw=0x");
    puthex(e0); puts(" 0x"); puthex(e1); puts(" 0x"); puthex(e4);
    puts("\r\n[ctx]   deq=0x");
    puthex(((uint64_t)e3 << 32) | (uint64_t)(e2 & 0xFFFFFFF0U));
    puts(" dcs="); putdec(e2 & 1U);
    /* EP State: 0=Disabled 1=Running 2=Halted 3=Stopped 4=Error */
    puts("\r\n[ctx]   epstate="); putdec(e0 & 7U);
    puts(" cerr="); putdec((e1 >> 1) & 3U);
    puts(" eptype="); putdec((e1 >> 3) & 7U);
    puts(" mps="); putdec((e1 >> 16) & 0xFFFFU);
    puts(" avgtrb="); putdec(e4 & 0xFFFFU);
    puts("\r\n");
}


/* ---- 止まった瞬間を丸ごと写す ----------------------------------------------
 *
 * **recovery より前に、一度だけ。**recovery はリングを作り直すので、
 * 最初に壊れた状態を消してしまう。1 回の電源投入で最大の情報を取るには
 * 「最初の timeout を凍らせてから直す」しかない (codex 相談 2026-08-20 (e))。
 *
 * 出すもの:
 *   1. DCBAA から引いた slot の EP0 文脈 (EP State / TR Dequeue / DCS)
 *   2. こちら側のリングの状態と、積んだ 3 つの TRB の生 dword
 *   3. イベントリングの取り出し位置と、そこから 8 個の生 TRB
 *   4. 置き場の中身と各カウンタ
 *   5. USBSTS / USBCMD / IMAN / ERDP / ERSTBA / ERSTSZ / CRCR */
static void xhci_freeze_snapshot(const char* tag, uint8_t slot_id, uint32_t idx0) {
    uint64_t ring = g_ep0_slot[slot_id].ring_phys;
    uint32_t i;

    puts("\r\n===== [snap] "); puts(tag);
    puts(" slot="); putdec(slot_id);
    puts(" =====\r\n");

    /* 1. EP0 文脈 (DCBAA から引く) */
    xhci_dump_dev_ctx("snap", xhci_dev_ctx_of(slot_id));

    /* 2. こちら側のリングと、積んだ TRB */
    puts("[snap] ring=0x"); puthex(ring);
    puts(" idx0="); putdec(idx0);
    puts(" enq="); putdec(g_ep0_slot[slot_id].enq_idx);
    puts(" cycle="); putdec(g_ep0_slot[slot_id].cycle);
    puts("\r\n");
    if (ring) {
        volatile uint32_t* r = (volatile uint32_t*)USB_VIRT((void*)ring);
        for (i = 0; i < 3U; i++) {
            uint32_t t = (idx0 + i) % 255U;
            puts("[snap] trb["); putdec(t); puts("] 0x");
            puthex(r[t * 4 + 0]); puts(" 0x");
            puthex(r[t * 4 + 1]); puts(" 0x");
            puthex(r[t * 4 + 2]); puts(" 0x");
            puthex(r[t * 4 + 3]); puts("\r\n");
        }
        /* **終端の手前 8 個。**飛び越したときに書かれずに残る隙間が
         * ここに当たる。サイクルビットが xHC の CCS と違えば、
         * xHC はそこで止まって Link まで到達しない */
        for (i = 248U; i <= 255U; i++) {
            puts("[snap] tail["); putdec(i); puts("] 0x");
            puthex(r[i * 4 + 0]); puts(" 0x");
            puthex(r[i * 4 + 3]);
            puts(" type="); putdec((r[i * 4 + 3] >> 10) & 0x3FU);
            puts(" cy="); putdec(r[i * 4 + 3] & 1U);
            puts("\r\n");
        }
    }

    /* 3. イベントリングの生データ */
    puts("[snap] evt idx="); putdec(g_evt_dequeue_idx);
    puts(" cycle="); putdec(g_evt_cycle);
    puts(" ring=0x"); puthex(g_event_ring_phys);
    puts("\r\n");
    if (g_event_ring_phys) {
        volatile uint32_t* e = (volatile uint32_t*)USB_VIRT((void*)g_event_ring_phys);
        for (i = 0; i < 8U; i++) {
            uint32_t t = (g_evt_dequeue_idx + i) % XHCI_EVT_RING_TRBS;
            puts("[snap] evt["); putdec(t); puts("] 0x");
            puthex(e[t * 4 + 0]); puts(" 0x");
            puthex(e[t * 4 + 1]); puts(" 0x");
            puthex(e[t * 4 + 2]); puts(" 0x");
            puthex(e[t * 4 + 3]);
            puts(" type="); putdec((e[t * 4 + 3] >> 10) & 0x3FU);
            puts(" cy="); putdec(e[t * 4 + 3] & 1U);
            puts("\r\n");
        }
    }

    /* 4. 置き場 */
    puts("[snap] stash count="); putdec(g_evt_stash_count);
    puts(" peak="); putdec(g_evt_stash_peak);
    puts(" drop="); putdec(g_evt_stash_dropped);
    puts(" pscd="); putdec(g_evt_pscd_count);
    puts("\r\n");
    for (i = 0; i < g_evt_stash_count; i++) {
        uint32_t k = (g_evt_stash_head + i) % XHCI_EVT_STASH_CAP;
        puts("[snap] stash["); putdec(i); puts("] type=");
        putdec((g_evt_stash[k].d[3] >> 10) & 0x3FU);
        puts(" slot="); putdec((g_evt_stash[k].d[3] >> 24) & 0xFFU);
        puts(" ep="); putdec((g_evt_stash[k].d[3] >> 16) & 0x1FU);
        puts(" cc="); putdec((g_evt_stash[k].d[2] >> 24) & 0xFFU);
        puts("\r\n");
    }

    /* 5. レジスタ */
    if (g_op_regs) {
        uint32_t sts = mmio_read32(g_op_regs, 0x04);
        puts("[snap] usbcmd=0x"); puthex(mmio_read32(g_op_regs, 0x00));
        puts(" usbsts=0x"); puthex(sts);
        puts(" hch="); putdec(sts & 1U);
        puts(" hse="); putdec((sts >> 2) & 1U);
        puts(" eint="); putdec((sts >> 3) & 1U);
        puts(" hce="); putdec((sts >> 12) & 1U);
        puts(" cnr="); putdec((sts >> 11) & 1U);
        puts("\r\n[snap] crcr=0x"); puthex(mmio_read32(g_op_regs, 0x18));
        puts("\r\n");
    }
    if (g_rt_regs) {
        puts("[snap] iman=0x"); puthex(mmio_read32(g_rt_regs, 0x20 + 0x00));
        puts(" erstsz=0x"); puthex(mmio_read32(g_rt_regs, 0x20 + 0x08));
        puts(" erstba=0x"); puthex(mmio_read32(g_rt_regs, 0x20 + 0x10));
        puts(" erdp=0x"); puthex(mmio_read32(g_rt_regs, 0x20 + 0x18));
        puts("\r\n");
    }
    puts("===== [snap] end =====\r\n");
}

/* ---- 完了コード (Completion Code) のうち、実機で出会ったもの ---------------
 *
 * 出典は xHCI 1.2 の Table 6-4 / 4.17.4。名前は Linux の
 * 「規格の名前に合わせる」改名 (COMP_* マクロ) で裏を取った (E-3, 2026-08-22)。
 *
 *    1  Success
 *    3  Babble Detected Error      相手のパケットが MPS より長い
 *    4  USB Transaction Error
 *    6  Stall Error
 *   19  Context State Error        文脈が期待した状態にない
 *   24  Command Ring Stopped
 *   25  Command Aborted
 *   26  Stopped                    Stop Endpoint で止めた
 *   27  Stopped - Length Invalid   同上。**ただし転送長が無効**
 *   28  Stopped - Short Packet
 *
 * **cc=27 のとき residual を読んではいけない。**「止めたので長さは
 * 当てにするな」という意味のコードで、失敗ではない。復帰処理
 * (xhci_ep0_recover_stall) が Stop Endpoint を投げた結果として、
 * 保留中だった転送がこれを返す。**新しい不具合ではない** */

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
    if (slot_id == 0 || g_ep0_slot[slot_id].ring_phys == 0) return -1;

    /* **直す前後の EP 状態と、各コマンドの完了コードを必ず残す。**
     * Reset Endpoint は Halted のときしか成功しない (それ以外は
     * Context State Error)。握り潰すと「直そうとして何もしなかった」が
     * ログから消える (codex 相談 (c)) */
    /* **状態で使うコマンドが違う。**
     *   Halted  → Reset Endpoint で落としてから Set TR Dequeue
     *   Running → Stop Endpoint で止めてから Set TR Dequeue
     *   Stopped / Error → そのまま Set TR Dequeue
     * Reset Endpoint は Halted 以外だと Context State Error (cc=19) で
     * 何も起きない。今まで Running で詰まったときに「直そうとして何も
     * していなかった」(codex 相談 2026-08-20 (c)/(e)) */
    {
        uint32_t st = xhci_ep0_state(slot_id);
        uint32_t type;
        usb_arch_console_begin();
        puts("[recov] slot="); putdec(slot_id);
        puts(" epstate="); putdec(st);
        puts(st == 1U ? " (Running -> Stop Endpoint)\r\n" :
             st == 2U ? " (Halted -> Reset Endpoint)\r\n" :
                        " (Stopped/Error -> issuing Set TR Dequeue as-is)\r\n");
        usb_arch_console_end();

        if (st == 1U || st == 2U) {
            /* 15 = Stop Endpoint / 14 = Reset Endpoint */
            type = (st == 2U) ? 14U : 15U;
            cmd_ptr = xhci_cmd_submit(0, 0, 0,
                                      (type << 10) | (1U << 16) | ((uint32_t)slot_id << 24));
            if (!cmd_ptr) return -2;
            if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) {
                puts("[recov] no completion event\r\n");
                return -3;
            }
            puts(st == 2U ? "[recov] Reset Endpoint cc=" : "[recov] Stop Endpoint cc=");
            putdec(cc);
            puts(cc == 1 ? "  ok\r\n" : "  *** failed (19=Context State Error)\r\n");
            if (cc != 1) return -4;
        }
    }

    xhci_ring_reset(g_ep0_slot[slot_id].ring_phys);
    g_ep0_slot[slot_id].enq_idx = 0;
    g_ep0_slot[slot_id].cycle = 1;
    USB_MB();

    cc = 0xFF;
    cmd_ptr = xhci_cmd_submit((uint32_t)((g_ep0_slot[slot_id].ring_phys & ~0xFULL) | 1ULL),
                              (uint32_t)(g_ep0_slot[slot_id].ring_phys >> 32), 0,
                              (16U << 10) | (1U << 16) | ((uint32_t)slot_id << 24));
    if (!cmd_ptr) return -5;
    if (xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot) < 0) {
        puts("[recov] no completion event for Set TR Dequeue\r\n");
        return -6;
    }
    puts("[recov] Set TR Dequeue cc="); putdec(cc);
    puts(cc == 1 ? "  ok\r\n" : "  *** failed\r\n");
    xhci_dump_dev_ctx("recov-post", xhci_dev_ctx_of(slot_id));
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
        usb_arch_console_begin();
        puts("[usb] hub port ");
        putdec(p);
        puts(" status=0x");
        puthex(st);
        puts((st & HUB_PORT_CONNECTION) ? "  connected\r\n" : "\r\n");
        usb_arch_console_end();
        if (!(st & HUB_PORT_CONNECTION)) continue;

        (void)usb_hub_port_feature(slot_id, p, HUB_FEAT_C_CONNECTION, 0);

        /* リセットして有効にする。**ルートポートと同じで、接続だけでは
         * Address Device は通らない** */
        (void)usb_hub_reset_port(slot_id, p, &st);

        usb_arch_console_begin();
        puts("[usb] hub port ");
        putdec(p);
        puts(" reset -> status=0x");
        puthex(st);
        puts((st & HUB_PORT_ENABLE) ? "  became enabled\r\n" : "  *** did not become enabled\r\n");
        usb_arch_console_end();
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
            puts("[usb] GET_DESCRIPTOR(Config hdr) rc=");
            putdec((uint64_t)(uint32_t)(-rc));
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
            puts("[usb] GET_DESCRIPTOR(Config body) rc=");
            putdec((uint64_t)(uint32_t)(-rc));
            puts(" total=");
            putdec(total_len);
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
        ic[off + 2] = (uint32_t)((g_ep0_slot[slot_id].ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_ep0_slot[slot_id].ring_phys >> 32);
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
        puts("[usb] Configure Endpoint(BULK) cc=");
        putdec(cc);
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
    xhci_ring_enqueue_advance(ring_phys, idx, cycle, 1);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, dci, trb_phys, XHCI_WAIT_FULL, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] bulk ");
        puts(in_dir ? "IN" : "OUT");
        puts(" dci=");
        putdec(dci);
        puts(" cc=");
        putdec(cc);
        puts(" residual=");
        putdec(residual);
        puts(" len=");
        putdec(len);
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
        ic[off + 2] = (uint32_t)((g_ep0_slot[slot_id].ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_ep0_slot[slot_id].ring_phys >> 32);
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
        puts("[usb] Configure Endpoint(INT) cc=");
        putdec(cc);
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

static int usb_hid_keyboard_poll_owned(uint8_t report[8]) {
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    /* **抜き差しの確認中は触らない。**イベントリングを取り合うと壊れる */
    if (g_usb_busy) return 1;

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
        xhci_ring_enqueue_advance(g_int_ring_phys, &g_int_enqueue_idx, &g_int_cycle, 1);
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

/* 外から呼ぶ口 (D-5)。**取れなければ「まだ来ていない」として戻る** —
 * 呼ぶのはタイマ割り込みで、次の tick でまた来る */
int usb_hid_keyboard_poll(uint8_t report[8]) {
    int ret;
    if (!g_usb_kbd_ready || !report) return -1;
    if (!usb_owner_try_take()) return 1;
    ret = usb_hid_keyboard_poll_owned(report);
    usb_owner_release();
    return ret;
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
        puthex_n(g_usb_bulk_out_ep, 2);
        puts(" in=0x");
        puthex_n(g_usb_bulk_in_ep, 2);
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

/* ---- A-2: Port Status Change Event の後始末 -------------------------------
 *
 * ルートポートの状態が変わると xHC は type 34 のイベントを上げる。**変化
 * ビットを落とすまで、xHC は同じ変化を上げ続ける。**
 *
 * ポーリングのうちは「置き場を埋めるだけ」で済んでいたので、数えて落として
 * いた。**割り込みにすると話が変わる。**INTx はレベル駆動なので、発生源を
 * 消さないと戻った瞬間にまた上がる — 割り込み嵐になって何も進まない。
 *
 * **読んだ値をそのまま書き戻さない。**PORTSC_KEEP の注釈のとおり、PED や
 * 変化ビットを書き戻すと「有効にしようとして無効にする」ことになる。
 * **立っている変化ビットだけ**を 1 で書いて落とす */
static void xhci_ack_port_change(uint8_t port) {
    uint32_t o, v, chg;
    if (!g_op_regs || port == 0 || port > g_xhci_max_ports) return;

    o = xhci_portsc_off(port);
    v = mmio_read32(g_op_regs, o);
    /* bits 17..23 = CSC / PEC / WRC / OCC / PRC / PLC / CEC。すべて RW1C */
    chg = v & (0x7FU << 17);
    if (!chg) return;

    g_evt_pscd_port = port;
    mmio_write32(g_op_regs, o, (v & PORTSC_KEEP) | chg);
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
            puts(" name=0x");
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
    usb_arch_console_begin();
    puts("[usb] port ");
    putdec(port);
    puts(" (USB");
    putdec(major);
    puts(") reset -> portsc=0x");
    puthex(portsc);
    puts((portsc & PORTSC_PED) ? "  became enabled\r\n" : "  *** did not become enabled\r\n");
    usb_arch_console_end();

    return (portsc & PORTSC_PED) ? 0 : -1;
}

/* ---- ハブの先のデバイスを掴む ---------------------------------------------
 *
 * **起動時の列挙と、抜き差しでの再列挙の両方から呼ぶ。**呼ぶ側は
 * 「ハブの何番ポートに、どの速度の何かが居る」ところまでを確かめてから
 * 渡す (usb_hub_find_device / 抜き差しの検出)。
 *
 * 中身は usb_init() に埋まっていたものをそのまま移した。**入れ子も含めて
 * ロジックは変えていない** — 再列挙の入口を作るのが目的で、動きを変えると
 * 退行したときに切り分けられなくなる。
 *
 * 成功すると g_xhci_slot_id がそのデバイスを指す。
 * 戻り値: 0 = 掴めた / -1 = だめだった */
static int usb_hub_attach_port(uint8_t hub_slot, uint8_t hub_port, uint32_t dev_speed) {
    int rc = -1;
    uint8_t dev_slot = 0;
    /* 高速ハブの先の低速/全速デバイスは TT が要る */
    uint8_t tt_slot = (dev_speed < 3) ? hub_slot : 0;
    uint8_t tt_port = (dev_speed < 3) ? hub_port : 0;

    if (xhci_cmd_enable_slot(&dev_slot) == 0) {
        uint32_t route = (uint32_t)hub_port & 0xFU; /* Route String 1 段目 */
        int ad2 = xhci_cmd_address_device_full(
            dev_slot, g_xhci_port_id, route,
            dev_speed, tt_slot, tt_port, 0);
        puts("[usb] hub downstream Address Device slot=");
        putdec(dev_slot);
        puts(" port=");
        putdec(hub_port);
        puts(" speed=");
        putdec(dev_speed);
        if (ad2 == 0) {
            g_xhci_slot_id = dev_slot;
            rc = 0;
            puts("  ok\r\n");

            /* ---- 全速の相手は EP0 の MPS を後から入れ替える ----
             *
             * **EP0 の最大パケット長は繋いでみるまで
             * 分からない** (8/16/32/64)。8 と決め打ちで
             * 先頭 8 バイトだけ読んで確かめ、
             * **Evaluate Context (A1) で入れ替える。**
             * 高速以上は 64 で一意に決まるので要らない。
             *
             * **8 バイトで打ち切ることは相手を壊さない。**
             * wLength=8 と言って 8 バイト受け取るのは
             * 標準的な列挙手順で、Linux の hub_port_init()
             * も同じことをする。
             *
             * **ポートを再リセットしてやり直す必要も無い。**
             * 以前ここにあった再リセットの経路は、
             * 原因が制御転送の TD の作り方だと分かった時点で
             * 消した (2026-08-19) */
            if (dev_speed < 3U) {
                memzero(USB_VIRT((void*)g_ep0_buf_phys), 16);
                if (xhci_ep0_control_in(
                        dev_slot,
                        usb_setup_packet(0x80, 0x06, 0x0100, 0, 8),
                        g_ep0_buf_phys, 8) == 0) {
                    volatile uint8_t* d =
                        (volatile uint8_t*)USB_VIRT((void*)g_ep0_buf_phys);
                    uint32_t m = d[7]; /* bMaxPacketSize0 */
                    /* **読めた 8 バイトをそのまま出す。**
                     * bMaxPacketSize0 が本物かどうかは、
                     * bLength=18 / bDescriptorType=1 が
                     * 揃っているかを見ないと言えない */
                    {
                        uint32_t k;
                        puts("[usb] dev desc[0..7]");
                        for (k = 0; k < 8U; k++) {
                            puts(" ");
                            puthex_byte(d[k]);
                        }
                        puts("\r\n");
                    }
                    if (d[1] != 1 ||
                        (m != 8 && m != 16 && m != 32 && m != 64)) {
                        puts("[usb] first 8 bytes are not a descriptor\r\n");
                    } else if (m != g_usb_ep0_mps) {
                        int er = xhci_cmd_evaluate_context_mps(dev_slot, m);
                        puts("[usb] ep0 mps ");
                        putdec(g_usb_ep0_mps);
                        puts(" -> ");
                        putdec(m);
                        puts(er == 0 ? "  ok\r\n" : "  *** failed\r\n");
                        if (er == 0) g_usb_ep0_mps = (uint16_t)m;
                    }
                }
            }

            /* **記述子を読む前に Configure Endpoint は要らない。**
             * Address Device が通った時点で EP0 は Running。
             * ハブと違ってポート要求を出すわけでもないので、
             * Configured へ進める理由が無い (2026-08-18 codex 相談) */
            int gd2 = xhci_ep0_get_device_descriptor_retry(dev_slot, 3);
            puts("[usb] hub downstream GET_DESCRIPTOR(Device) ");
            if (gd2 == 0) {
                puts("OK vid=0x");
                puthex_n(g_usb_vid, 4);
                puts(" pid=0x");
                puthex_n(g_usb_pid, 4);
                puts(" class=0x");
                puthex_n(g_usb_dev_class, 2);
                puts("\r\n");
            } else {
                puts("failed code=");
                putdec((uint64_t)(uint32_t)(-gd2));
                puts("\r\n");
            }
        } else {
            puts("  *** failed code=");
            putdec((uint64_t)(uint32_t)(-ad2));
            puts("\r\n");
        }
    }
    return rc;
}

/* ---- 抜き差し (B-1) -------------------------------------------------------
 *
 * ハブの各ポートに GET_STATUS を投げ、**bit16 C_PORT_CONNECTION** が
 * 立っていれば「前に見たときから変わった」。落としてから、接続なら
 * 列挙し、切断ならスロットを返す。
 *
 * **タイマ割り込みからは呼べない。**制御転送は 1 回 ms 単位かかり、
 * aarch64_kbd_tick() はタイマ IRQ から呼ばれている。task_idle_loop()
 * から、**500ms に 1 回**に絞って呼ぶ (kernel/sched.c)。
 *
 * **キーボードのイベントと同じリングを使う。**usb_hid_keyboard_poll()
 * は割り込み TRB を積んだまま戻るので、ここで制御転送を投げている間に
 * キーのイベントが来る。捨てないための土台が B-1a の置き場 */
static uint64_t g_hub_poll_next_ms = 0;
static uint8_t  g_hub_dev_port = 0;    /* いま掴んでいるハブのポート。0 = 無し */
static uint32_t g_hub_poll_count = 0;
static int      g_hub_scan_told = 0;
static int      g_hub_poll_announced = 0;
/* **前回見えていた接続の有無。**変化ビット (C_PORT_CONNECTION) に頼らず、
 * 接続そのものの変わり目で判断する。ビットの位置を読み違えていても
 * 取りこぼさない */
#define HUB_MAX_PORTS 16
static uint8_t  g_hub_port_conn[HUB_MAX_PORTS + 1];
static uint32_t g_hub_attach_count = 0;
static uint32_t g_hub_detach_count = 0;

/* 掴んでいたデバイスを手放す。**スロットを返して、キーボードを止める** */
static void usb_hotplug_release(void) {
    uint8_t slot = g_xhci_slot_id;

    g_usb_kbd_ready = 0;
    g_int_outstanding = 0;      /* 積んだままの TRB は相手ごと消えた */
    g_usb_hid_if_ready = 0;
    g_hub_dev_port = 0;

    if (slot != 0 && slot != g_hub_slot_id) {
        int dr = xhci_cmd_disable_slot(slot);
        puts("[usb] hotplug: slot ");
        putdec(slot);
        puts(dr == 0 ? " returned\r\n" : " could not return\r\n");
        /* **返したスロットの EP0 リングも忘れる。**残しておくと、
         * 同じ番号が再び割り当てられたときに古いリングを使ってしまう */
        g_ep0_slot[slot].ring_phys = 0;
        g_ep0_slot[slot].enq_idx = 0;
        g_ep0_slot[slot].cycle = 1;
    }
    /* **ハブ自身のスロットに戻す。**制御転送の宛先が無いと次が投げられない */
    g_xhci_slot_id = g_hub_slot_id;
}

static uint64_t g_usb_heartbeat_next_ms;   /* A-3 の要約を次に出す時刻 */

/* A-2 の 1 行。**較正の値を毎回添える** — 待ちの回数だけ見ても
 * それが何秒なのか分からないため */
static void xhci_wait_dump(const char* name, const xhci_wait_stat_t* s) {
    puts("[usb] A-2 wait ");
    puts(name);
    puts(": n=");
    putdec(s->calls);
    puts(" immediate=");
    putdec(s->immediate);
    puts(" max=");
    putdec(s->max_loops);
    /* **「回転」で出す。**A-1 の前は pump を呼んだ回数で、較正した
     * 221ns/回 を掛ければ時間になった。**いまは 1 回転が pump を
     * 含まないので、掛け算は成り立たない。**時間は max_ms を見る */
    puts(" spins  max ms=");
    putdec(s->max_ms);
    puts(" reached=");
    putdec(s->timeouts);
    if (s->timeouts) {
        puts("(");
        putdec(s->timeout_ms);
        puts("ms)");
    }
    puts(" limit=");
    putdec(XHCI_WAIT_MS);
    puts("ms\r\n");
}

static void usb_hotplug_poll_owned(void) {
    uint8_t p;
    uint64_t now;

    g_hub_poll_count++;

    /* **1 回目は必ず、ガードより前に言う。**ガードの後ろに置くと
     * 「呼ばれていない」と「ガードで弾かれている」が区別できない
     * (2026-08-20 codex 相談 (d)) */
    if (!g_hub_poll_announced) {
        g_hub_poll_announced = 1;
        puts("[usb] hotplug: entered ready=");
        putdec(g_usb_ready ? 1U : 0U);
        puts(" hub_slot=");
        putdec(g_hub_slot_id);
        puts(" ports=");
        putdec(g_hub_nbr_ports);
        puts(" dev_port=");
        putdec(g_hub_dev_port);
        puts("\r\n");
    }

    if (!g_usb_ready || g_hub_slot_id == 0 || g_hub_nbr_ports == 0) {
        /* **弾かれた理由も 1 回だけ出す** */
        static int told;
        if (!told) {
            told = 1;
            puts("[usb] hotplug: returned by guard ready=");
            putdec(g_usb_ready ? 1U : 0U);
            puts(" hub_slot=");
            putdec(g_hub_slot_id);
            puts(" ports=");
            putdec(g_hub_nbr_ports);
            puts("\r\n");
        }
        return;
    }

    now = USB_NOW_MS();
    if (now < g_hub_poll_next_ms) return;
    g_hub_poll_next_ms = now + 500ULL;

    /* ---- A-3: 60 秒ごとに 1 行だけ要約を出す ------------------------------
     *
     * 実機のシェルは busybox ash で、`usb` コマンド (usb_dump_status) が
     * 使えない。**起動時の 1 行だけでは長時間の様子が分からない。**
     *
     * 1 分 1 行なら何時間動かしてもログは埋まらない。見るのはこの 4 つ:
     *
     *   irq=   増え続けていれば割り込みが生きている。**止まったら A-1 が
     *          効いていない** (動きはポーリングに落ちるだけで壊れはしない)
     *   drop=  0 でなければイベントを取りこぼしている
     *   pscd=  増え続けるなら変化ビットを落とせていない (A-2 の失敗)
     *   wrap=  リングの一周。止まっていれば制御転送が流れていない */
    if (now >= g_usb_heartbeat_next_ms) {
        g_usb_heartbeat_next_ms = now + 60000ULL;
        /* **要約は数行まとめて 1 単位にする (M-2)。**中は出力だけで、
         * 待ちも return も無いので長く握らない */
        usb_arch_console_begin();
        puts("[usb] elapsed poll=");
        putdec(g_hub_poll_count);
        puts(" irq=");
        putdec(g_xhci_irq_count);
        puts("/");
        putdec(g_xhci_irq_events);
        puts(" stash=");
        putdec(g_evt_stash_count);
        puts("/peak=");
        putdec(g_evt_stash_peak);
        puts("/drop=");
        putdec(g_evt_stash_dropped);
        puts(" pscd=");
        putdec(g_evt_pscd_count);
        puts(" wrap=");
        putdec(g_ring_wrap_count);
        puts(" in/out=");
        putdec(g_hub_attach_count);
        puts("/");
        putdec(g_hub_detach_count);
        puts("\r\n");

        /* ---- V-1: fork の写しにどれだけ使っているか ---------------------
         *
         * **aarch64 は CoW が無く、fork でユーザーのページを全部写す。**
         * CoW を入れるかを「効くはず」ではなく数字で決めるための計器。
         * 実装が無いアーキでは弱いシンボルの空実装が使われ、0 が出る */
        {
            uint64_t fk = 0, fp = 0, fms = 0;
            aarch64_fork_stats(&fk, &fp, &fms);
            if (fk != 0) {
                puts("[fork] n=");
                putdec(fk);
                puts(" pages=");
                putdec(fp);
                puts(" ms=");
                putdec(fms);
                puts("\r\n");
            }
        }

        /* ---- A-2: 待ちの実測。block にするときの根拠 --------------------
         *
         *   n=      上限まで待つ気で入った回数
         *   即=     置き場に既にあって 1 回も回らなかった回数
         *   最大=   完了までに回った回数の最大 (較正で us に直したもの)
         *   最大ms= 経過時間の最大。**回数からの換算の答え合わせ**
         *   到達=   上限まで回って諦めた回数。0 でなければ上限が短い */
        xhci_wait_dump("transfer", &g_wait_xfer);
        xhci_wait_dump("command", &g_wait_cmd);

        /* **取り分とレジスタ。**割り込みが仕事をしていない理由の切り分け。
         * iman の bit0 (IP) が立ちっぱなしなら、こちらが落とせていない */
        puts("[usb] A-1 share irq=");
        putdec(g_xhci_irq_events);
        puts(" fallback=");
        putdec(g_selfpump_events);
        puts(" kbd=");
        putdec(g_kbdpump_events);
        puts(" entry eint=");
        putdec(g_irq_eint);
        puts("/none=");
        putdec(g_irq_noeint);
        puts("/pcd=");
        putdec(g_irq_pcd);
        if (g_op_regs && g_rt_regs) {
            puts(" usbsts=0x");
            puthex(mmio_read32(g_op_regs, 0x04));
            puts(" usbcmd=0x");
            puthex(mmio_read32(g_op_regs, 0x00));
            puts(" iman=0x");
            puthex(mmio_read32(g_rt_regs, 0x20 + 0x00));
        }
        puts("\r\n");
        usb_arch_console_end();
    }

    g_usb_busy = 1;
    for (p = 1; p <= g_hub_nbr_ports && p <= HUB_MAX_PORTS; p++) {
        uint32_t st = 0;
        uint8_t now_conn;
        int changed;
        int rc = usb_hub_get_port_status(g_hub_slot_id, p, &st);

        /* **最初の 1 巡だけ、4 ポート全部の結果を出す。**
         * 失敗を continue で流していたので、GET_STATUS が通っているのか
         * どうかすら分からなかった */
        if (!g_hub_scan_told) {
            usb_arch_console_begin();
            puts("[usb] hotplug: scan port ");
            putdec(p);
            puts(" rc=");
            putdec((uint64_t)(uint32_t)(-rc));
            puts(" st=0x");
            puthex(st);
            puts("\r\n");
            usb_arch_console_end();
            if (p >= g_hub_nbr_ports || p >= HUB_MAX_PORTS) g_hub_scan_told = 1;
        }

        if (rc < 0) {
            /* **失敗したら 1 巡を打ち切って間を空ける。**4 ポートぶん
             * 待ち切ると 500ms ごとに長々と回ることになる */
            g_hub_poll_next_ms = USB_NOW_MS() + 2000ULL;
            break;
        }

        /* **変化ビットは立っていたら落とす。**落とさないと立ったままになる。
         * ただし判断はこれに頼らない (下の接続の変わり目で見る) */
        changed = (st & HUB_C_PORT_CONNECTION) ? 1 : 0;
        if (changed) {
            (void)usb_hub_port_feature(g_hub_slot_id, p, HUB_FEAT_C_CONNECTION, 0);
        }

        now_conn = (st & HUB_PORT_CONNECTION) ? 1U : 0U;
        if (now_conn == g_hub_port_conn[p]) {
            /* **接続ビットが同じでも、変化ビットが立っていたら動きがあった。**
             * 500ms の間に「抜く→挿す」が終わると接続は元に戻るが、
             * 掴んでいるスロットと積んだままの TRB は相手ごと消えている。
             * 掴んでいるポートなら作り直す (codex 相談 (b)) */
            if (!changed || p != g_hub_dev_port) continue;
            puts("[usb] hotplug: port ");
            putdec(p);
            puts(" replugged (connect bit unchanged)\r\n");
            usb_hotplug_release();
            g_hub_detach_count++;
            /* このまま下の「挿された」経路へ落とす */
        }
        g_hub_port_conn[p] = now_conn;

        puts("[usb] hotplug: port ");
        putdec(p);
        puts(now_conn ? " plugged into status=0x" : " unplugged from status=0x");
        puthex(st);
        puts("\r\n");

        if (!now_conn) {
            if (g_hub_dev_port == p) {
                usb_hotplug_release();
                g_hub_detach_count++;
            }
            continue;
        }

        /* ---- 挿された ---------------------------------------------------- */

        /* **前のものが残っていたら先に手放す。**同じポートに挿し直された
         * ときは、抜けたことに気付かないまま挿入だけ見えることがある */
        if (g_hub_dev_port != 0) usb_hotplug_release();

        /* リセットして有効にする。**接続だけでは Address Device は通らない** */
        {
            uint32_t rst = 0;
            (void)usb_hub_reset_port(g_hub_slot_id, p, &rst);
            puts("[usb] hotplug: port ");
            putdec(p);
            puts(" reset -> status=0x");
            puthex(rst);
            puts((rst & HUB_PORT_ENABLE) ? "  became enabled\r\n"
                                         : "  *** did not become enabled\r\n");
            if (!(rst & HUB_PORT_ENABLE)) continue;
            st = rst;
        }

        {
            uint32_t sp = (st & HUB_PORT_LOW_SPEED)  ? 2U :
                          (st & HUB_PORT_HIGH_SPEED) ? 3U : 1U;
            if (usb_hub_attach_port(g_hub_slot_id, p, sp) != 0) {
                puts("[usb] hotplug: could not acquire\r\n");
                g_xhci_slot_id = g_hub_slot_id;
                continue;
            }
        }

        g_hub_dev_port = p;
        g_hub_attach_count++;

        /* 設定記述子を読んで HID を繋ぐ。**起動時と同じ手順** */
        if (xhci_ep0_get_config_descriptor(g_xhci_slot_id) == 0 && g_usb_hid_if_ready) {
            int hk = usb_hid_keyboard_init();
            puts("[usb] hotplug: HID keyboard ");
            if (hk == 0) {
                puts("ok ep=0x");
                puthex_n(g_usb_int_in_ep, 2);
                puts(" dci=");
                putdec(g_usb_int_in_dci);
                puts("\r\n");
            } else {
                puts("*** failed code=");
                putdec((uint64_t)(uint32_t)(-hk));
                puts("\r\n");
            }
        } else {
            puts("[usb] hotplug: could not read config descriptor / not HID\r\n");
        }
    }
    g_usb_busy = 0;
}

/* 外から呼ぶ口 (D-5)。**各 CPU の idle ループが呼ぶ** (kernel/sched.c)。
 * 誰かが触っている間は黙って戻る — 500ms 後にまた来る */
void usb_hotplug_poll(void) {
    if (!usb_owner_try_take()) return;
    usb_hotplug_poll_owned();
    usb_owner_release();
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
            puts("[usb] xHCI BAR0 above 4GiB is not mapped yet phys=0x");
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
                puts(" -- 4KB unusable. ring allocation is wrong\r\n");
            }
        }

        // Clear pending status bits before Run.
        mmio_write32(op, 0x04, 0xFFFFFFFFU);

        usbcmd = mmio_read32(op, 0x00);
        /* Run + INTE。**INTE を立てるだけでは何も来ない。**
         * ホスト側の口 (GIC / PIC) も開けないと「有効にしたつもり」になる。
         * それは下の usb_arch_irq_enable() が受け持つ (A-1) */
        mmio_write32(op, 0x00, usbcmd | 1U | (1U << 2)); // RS=1, INTE=1
        if (xhci_wait_bits(op, 0x04, (1U << 0), 0, 2000000) < 0) {
            puts("[usb] xHCI run timeout\r\n");
            return;
        }

        /* ---- 割り込みの口を開ける (A-1) --------------------------------
         *
         * **走り出してから開ける。**止まっている間に上がった表明を
         * 拾っても意味が無い。
         *
         * **保険は外していない。**待っている側は割り込みが来なくても
         * 自分で xhci_evt_pump() を回すので、配線が違っていても動きは
         * 今までのポーリングに戻るだけ */
        usb_arch_irq_enable();
        puts("[usb] opened the interrupt endpoint (see [usb] ports: irq= for count)\r\n");

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
        putdec((uint64_t)(uint32_t)(-cmd_probe));
        puts("\r\n");
    } else {
        uint8_t slot_id = 0;
        int es;
        /* **A-2: 空回りの実時間を測る。**列挙を始める前。ここなら
         * イベントリングは走っていて、まだ誰も待っていない */
        xhci_spin_calibrate();
        es = xhci_cmd_enable_slot(&slot_id);
        if (es == 0) {
            g_xhci_slot_id = slot_id;
            puts("[usb] Enable Slot OK slot=");
            putdec(slot_id);
            puts("\r\n");

            /* **接続のあるポートを、リセットして有効にしてから選ぶ。**
             * 接続があるだけでは Address Device は通らない (実機で
             * TRB Error になった)。全ポートの生の PORTSC も出しておく —
             * 無いと失敗の理由を推測でしか言えない */
            for (uint8_t p = 1; p <= g_xhci_max_ports; p++) {
                uint32_t portsc = mmio_read32(op, xhci_portsc_off(p));
                puts("[usb] portsc[");
                putdec(p);
                puts("]=0x");
                puthex(portsc);
                puts(" USB");
                putdec((p < XHCI_MAX_PORTS_TRACKED) ? g_port_major[p] : 0U);
                puts((portsc & PORTSC_CCS) ? "  connected\r\n" : "\r\n");
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
                    putdec(g_xhci_port_id);
                    puts("\r\n");

                    int ce = xhci_cmd_configure_endpoint(g_xhci_slot_id);
                    if (ce == 0) {
                        g_xhci_cfg_ready = 1;
                        puts("[usb] Configure Endpoint OK slot=");
                        putdec(g_xhci_slot_id);
                        puts("\r\n");
                    } else {
                        // For default EP0 path, many controllers allow proceeding
                        // after Address Device even if Configure Endpoint is rejected.
                        g_xhci_cfg_ready = 1;
                        puts("[usb] Configure Endpoint skipped code=");
                        putdec((uint64_t)(uint32_t)(-ce));
                        puts(" (continue with EP0)\r\n");
                    }

                    int gd = xhci_ep0_get_device_descriptor(g_xhci_slot_id);
                    if (gd == 0) {
                        puts("[usb] GET_DESCRIPTOR(Device) OK vid=0x");
                        puthex_n(g_usb_vid, 4);
                        puts(" pid=0x");
                        puthex_n(g_usb_pid, 4);
                        puts(" class=0x");
                        puthex_n(g_usb_dev_class, 2);
                        puts("\r\n");
                    } else {
                        puts("[usb] GET_DESCRIPTOR(Device) failed code=");
                        putdec((uint64_t)(uint32_t)(-gd));
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
                        puts("[usb] found a hub. probing downstream\r\n");

                        /* **ポート要求の前に設定を選ぶ。**Address 状態のまま
                         * SET_FEATURE を投げると実機の VL805 は STALL する */
                        {
                            int hc = usb_hub_configure(g_hub_slot_id);
                            puts("[usb] hub SET_CONFIGURATION ");
                            if (hc == 0) {
                                puts("ok\r\n");
                            } else {
                                puts("*** failed code=");
                                putdec((uint64_t)(uint32_t)(-hc));
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
                                puts("[usb] *** could not set Hub bit on slot\r\n");
                            }

                            if (usb_hub_find_device(g_hub_slot_id, nbr, &hub_port, &dev_speed) == 0) {
                                if (usb_hub_attach_port(g_hub_slot_id, hub_port, dev_speed) == 0) {
                                    /* **抜かれたことに気付けるよう、掴んだ
                                     * ポートを覚えておく** (B-1) */
                                    g_hub_dev_port = hub_port;
                                    if (hub_port <= HUB_MAX_PORTS) {
                                        g_hub_port_conn[hub_port] = 1;
                                    }
                                }
                            } else {
                                puts("[usb] nothing connected to any hub port\r\n");
                            }
                        } else {
                            puts("[usb] *** could not read hub descriptor\r\n");
                        }
                    }

                    int gc = xhci_ep0_get_config_descriptor(g_xhci_slot_id);
                    if (gc == 0 && g_usb_hid_if_ready) {
                        puts("[usb] HID keyboard if=");
                        putdec(g_usb_hid_if_number);
                        puts(" ep=0x");
                        puthex_n(g_usb_int_in_ep, 2);
                        puts(" mps=");
                        putdec(g_usb_int_in_mps);
                        puts(" interval=");
                        putdec(g_usb_int_in_interval);
                        puts(" dci=");
                        putdec(g_usb_int_in_dci);
                        puts("\r\n");
                    }
                    if (gc == 0) {
                        puts("[usb] GET_DESCRIPTOR(Config) MSC if class=0x");
                        puthex_n(g_usb_msc_if_class, 2);
                        puts(" sub=0x");
                        puthex_n(g_usb_msc_if_subclass, 2);
                        puts(" proto=0x");
                        puthex_n(g_usb_msc_if_proto, 2);
                        puts(" cfg=");
                        putdec(g_usb_cfg_value);
                        puts(" if=");
                        putdec(g_usb_msc_if_number);
                        puts(" epout=0x");
                        puthex_n(g_usb_bulk_out_ep, 2);
                        puts(" epin=0x");
                        puthex_n(g_usb_bulk_in_ep, 2);
                        puts("\r\n");
                    } else {
                        puts("[usb] GET_DESCRIPTOR(Config) failed code=");
                        putdec((uint64_t)(uint32_t)(-gc));
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
                            putdec((uint64_t)(uint32_t)(-sc));
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
                                    puts("[usb] MSC BOT ready blocks=");
                                    putdec(g_usb_block_count);
                                    puts(" block_size=");
                                    putdec(g_usb_block_size);
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
                                    putdec((uint64_t)(uint32_t)(-be));
                                    puts("\r\n");
                            }
                        }
                    }
                } else {
                    puts("[usb] Address Device failed code=");
                    putdec((uint64_t)(uint32_t)(-ad));
                    puts("\r\n");
                }
            }
        } else {
            puts("[usb] Enable Slot failed code=");
            putdec((uint64_t)(uint32_t)(-es));
            puts("\r\n");
        }
    }

    g_xhci_mmio = mmio_phys;
    g_usb_ready = 1;

    puts("[usb] xHCI capability mapped at phys=0x");
    puthex(mmio_phys);
    puts(" caplen=");
    putdec(caplen);
    puts(" hciver=0x");
    puthex_n(hciversion, 4);
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
    putdec(g_xhci_slot_id);
    puts(" port=");
    putdec(g_xhci_port_id);
    puts(" sp=");
    putdec(g_scratchpad_count);
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
    putdec(g_xhci_max_ports);
    puts(" connected=");
    putdec(connected);
    puts(" enabled=");
    putdec(enabled);
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
    /* **イベントの置き場の使われ方。**溢れていれば取りこぼしている */
    puts(" hotplug=");
    putdec(g_hub_poll_count);
    puts("poll/");
    putdec(g_hub_attach_count);
    puts("in/");
    putdec(g_hub_detach_count);
    puts("out port=");
    putdec(g_hub_dev_port);
    puts(" evtstash=");
    putdec(g_evt_stash_count);
    puts("/peak=");
    putdec(g_evt_stash_peak);
    puts("/drop=");
    putdec(g_evt_stash_dropped);
    puts("/pscd=");
    putdec(g_evt_pscd_count);
    /* **割り込みが本当に来ているか (A-1)。**「来た数/拾ったイベント数」。
     * 0/0 のままなら配線が違う。その場合でも待っている側が自分で
     * 吸い出すので、動きはポーリングのまま止まらない */
    puts(" irq=");
    putdec(g_xhci_irq_count);
    puts("/");
    putdec(g_xhci_irq_events);
    puts(" slot=");
    putdec(g_xhci_slot_id);
    puts(" port=");
    putdec(g_xhci_port_id);
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
    /* **イベントの置き場の使われ方。**溢れていれば取りこぼしている */
    puts(" hotplug=");
    putdec(g_hub_poll_count);
    puts("poll/");
    putdec(g_hub_attach_count);
    puts("in/");
    putdec(g_hub_detach_count);
    puts("out port=");
    putdec(g_hub_dev_port);
    puts(" evtstash=");
    putdec(g_evt_stash_count);
    puts("/peak=");
    putdec(g_evt_stash_peak);
    puts("/drop=");
    putdec(g_evt_stash_dropped);
    puts("/pscd=");
    putdec(g_evt_pscd_count);
    /* **割り込みが本当に来ているか (A-1)。**「来た数/拾ったイベント数」。
     * 0/0 のままなら配線が違う。その場合でも待っている側が自分で
     * 吸い出すので、動きはポーリングのまま止まらない */
    puts(" irq=");
    putdec(g_xhci_irq_count);
    puts("/");
    putdec(g_xhci_irq_events);
    puts(" slot=");
    putdec(g_xhci_slot_id);
    puts(" port=");
    putdec(g_xhci_port_id);
    puts(" mmio=0x");
    puthex(g_xhci_mmio);
    puts(" dcbaa=0x");
    puthex(g_dcbaap_phys);
    puts(" cr=0x");
    puthex(g_cmd_ring_phys);
    puts(" er=0x");
    puthex(g_event_ring_phys);
    puts(" sp=");
    putdec(g_scratchpad_array_phys);
    puts(" inctx=0x");
    puthex(g_input_ctx_phys);
    puts(" outctx=0x");
    puthex(g_output_ctx_phys);
    /* **いま掴んでいるデバイスの EP0 リング。**スロットごとに持つように
     * なったので、どのスロットのものかも出す */
    puts(" ep0slot=");
    putdec(g_xhci_slot_id);
    puts(" ep0=0x");
    puthex(g_ep0_slot[g_xhci_slot_id].ring_phys);
    puts(" vid=0x");
    puthex_n(g_usb_vid, 4);
    puts(" pid=0x");
    puthex_n(g_usb_pid, 4);
    puts(" cls=0x");
    puthex_n(g_usb_dev_class, 2);
    puts(" sub=0x");
    puthex_n(g_usb_dev_subclass, 2);
    puts(" pr=0x");
    puthex_n(g_usb_dev_proto, 2);
    puts(" ifc=0x");
    puthex_n(g_usb_msc_if_class, 2);
    puts(" ifs=0x");
    puthex_n(g_usb_msc_if_subclass, 2);
    puts(" ifp=0x");
    puthex_n(g_usb_msc_if_proto, 2);
    puts(" cfgv=");
    putdec(g_usb_cfg_value);
    puts(" ifn=");
    putdec(g_usb_msc_if_number);
    puts(" epout=0x");
    puthex_n(g_usb_bulk_out_ep, 2);
    puts(" epin=0x");
    puthex_n(g_usb_bulk_in_ep, 2);
    puts(" dciout=");
    putdec(g_usb_bulk_out_dci);
    puts(" dciin=");
    putdec(g_usb_bulk_in_dci);
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    /* **イベントの置き場の使われ方。**溢れていれば取りこぼしている */
    puts(" hotplug=");
    putdec(g_hub_poll_count);
    puts("poll/");
    putdec(g_hub_attach_count);
    puts("in/");
    putdec(g_hub_detach_count);
    puts("out port=");
    putdec(g_hub_dev_port);
    puts(" evtstash=");
    putdec(g_evt_stash_count);
    puts("/peak=");
    putdec(g_evt_stash_peak);
    puts("/drop=");
    putdec(g_evt_stash_dropped);
    puts("/pscd=");
    putdec(g_evt_pscd_count);
    puts(" irq=");
    putdec(g_xhci_irq_count);
    puts("/");
    putdec(g_xhci_irq_events);
    puts(" iq=");
    puts(g_usb_msc_inquiry_ok ? "ok" : "fail");
    puts(" cap=");
    puts(g_usb_msc_capacity_ok ? "ok" : "fail");
    puts(" blocks=");
    putdec(g_usb_block_count);
    puts(" blksz=");
    putdec(g_usb_block_size);
    puts("\r\n");
}
