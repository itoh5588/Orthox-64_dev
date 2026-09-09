/*
 * virtio-mmio 版 virtio-net (N-9 手1)。**ポーリング方式。**
 *
 * kernel/aarch64/virtio_blk_mmio.c (M4-1) を土台に、MMIO のスロット走査・
 * legacy 初期化・vring 組み立てをそのまま流用した。kernel/virtio_net.c
 * (x86、PCI legacy) のキュー構成・パケット送受信ロジックも流用している。
 * どちらも include/virtio.h の struct virtio_queue / vring 定義を共有する
 * ので、実際に変えたのは「PCI I/O ポートで叩く」を「MMIO レジスタを
 * 上位 VA 経由で叩く」に差し替えた部分だけ。
 *
 * **完了は割り込みではなくポーリングだけ。** virtio_blk_mmio.c は DTB で
 * 確かめられた INTID を GIC に登録して割り込みで完了を待てるが、そちらの
 * 割り込み経路 (kernel/aarch64/boot.c の aarch64_irq_handler) には
 * virtio-net 用の分岐が無い。DHCP/ping が動くかを確かめるのが目的なので、
 * まずポーリングだけで確認し、要ると分かってから割り込みを足す
 * ([[measure-before-optimizing]])。
 *
 * `net_poll()` は kernel/aarch64/timer.c の CPU 0 の tick から呼ぶ
 * (kbd_tick/sound_tick と同じ場所)。
 *
 * ★ 実機で効く注意点 (virtio_blk_mmio.c と同じ):
 *   vring も RX/TX バッファも、Raspberry Pi 4 実機では DMA コヒーレンシの
 *   手当てが要る。QEMU のスモークでは検出できない。実機は Pi 4 の
 *   実チップ (BCM GENET) に別ドライバが要るので、この経路そのものが
 *   通らない — QEMU virt での確認専用 (N-9 の手2 で GENET に進む)。
 */
#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "virtio_net.h"
#include "spinlock.h"
#include "aarch64/boot.h"
#include "aarch64/vm.h"

uint64_t aarch64_pmm_alloc(uint64_t pages);
void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);

static void put(const char* s) { aarch64_uart_puts(s); }

/* emmc2.c と同じ形。詰めた桁数で出す (16 進はレジスタと違って読みたいのは
 * 値そのものなので、先頭 0 を落とさない固定桁で出す) */
static void putdec(uint64_t v) {
    char buf[21];
    int i = 0;
    if (v == 0) { put("0"); return; }
    while (v) { buf[i++] = (char)('0' + (uint32_t)(v % 10U)); v /= 10U; }
    while (i--) aarch64_uart_putchar(buf[i]);
}

/* "0x" は付けない版。MAC の 1 バイトずつを "52:54:00:..." のように
 * 桁区切りで出すのに使う (usb.c の puthex_n とは "0x" の扱いが違うので注意) */
static void puthex_bare(uint64_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) aarch64_uart_putchar(d[(v >> (i * 4)) & 0xf]);
}


#define VIRTIO_MMIO_MAGIC_VALUE      0x000
#define VIRTIO_MMIO_VERSION          0x004
#define VIRTIO_MMIO_DEVICE_ID        0x008
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE  0x028
#define VIRTIO_MMIO_QUEUE_SEL        0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_MMIO_QUEUE_NUM        0x038
#define VIRTIO_MMIO_QUEUE_ALIGN      0x03c
#define VIRTIO_MMIO_QUEUE_PFN        0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS           0x070
#define VIRTIO_MMIO_CONFIG           0x100

#define VIRTIO_MMIO_MAGIC            0x74726976U   /* "virt" */
#define VIRTIO_MMIO_DEVICE_ID_NET    1

#define VNET_QUEUE_MAX     256
#define VNET_PAGE_SIZE     4096U
#define VIRTIO_RX_QUEUE    0
#define VIRTIO_TX_QUEUE    1
#define VIRTIO_RX_SLOTS    8
#define VIRTIO_TX_SLOT     0
#define VIRTIO_FRAME_MAX   1514
#define VIRTIO_RX_BUF_SIZE 2048

struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

static volatile uint8_t* g_base;   /* 上位 VA */
static uint64_t g_base_pa;
static uint32_t g_slot;

static struct virtio_queue g_rxq;
static struct virtio_queue g_txq;
static uint8_t* g_rx_bufs[VIRTIO_RX_SLOTS];
static uint64_t g_rx_buf_phys[VIRTIO_RX_SLOTS];
static uint8_t* g_tx_buf;
static uint64_t g_tx_buf_phys;
static uint8_t g_mac[6];
static int g_ready = 0;
static int g_tx_busy = 0;
static virtio_net_rx_cb_t g_rx_cb = 0;
static spinlock_t g_vnet_lock;

static void mmio_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_base + off) = v;
}
static uint32_t mmio_r32(uint32_t off) {
    return *(volatile uint32_t*)(g_base + off);
}

static void vnet_memset(void* p, uint8_t v, uint64_t n) {
    uint8_t* d = (uint8_t*)p;
    while (n--) *d++ = v;
}

/* legacy のキュー設定。virtio_blk_mmio.c の vblk_queue_setup と同じ形だが、
 * **キュー番号を選んでから使う** (RX=0/TX=1 の 2 本が要る) */
static int vnet_queue_setup(uint32_t queue_idx, struct virtio_queue* q, uint16_t want_size) {
    uint32_t queue_max, queue_size, avail_bytes, used_off, used_bytes, bytes, pages;
    uint64_t ring_pa;

    mmio_w32(VIRTIO_MMIO_QUEUE_SEL, queue_idx);
    queue_max = mmio_r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max == 0) return -1;
    queue_size = queue_max < want_size ? queue_max : want_size;
    if (queue_size > VNET_QUEUE_MAX) queue_size = VNET_QUEUE_MAX;

    avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + queue_size));
    used_off = (uint32_t)((sizeof(struct vring_desc) * queue_size + avail_bytes +
                           (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    used_bytes = (uint32_t)(sizeof(uint16_t) * 3U +
                            sizeof(struct vring_used_elem) * queue_size);
    bytes = used_off + used_bytes;
    pages = (bytes + VNET_PAGE_SIZE - 1) / VNET_PAGE_SIZE;

    ring_pa = aarch64_pmm_alloc(pages);
    if (!ring_pa) return -1;

    q->queue_size = (uint16_t)queue_size;
    q->ring_phys = ring_pa;
    q->ring_virt = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(ring_pa);
    q->active_descs = 0;
    q->last_used_idx = 0;
    vnet_memset(q->ring_virt, 0, (uint64_t)pages * VNET_PAGE_SIZE);
    q->desc  = (struct vring_desc*)q->ring_virt;
    q->avail = (struct vring_avail*)(q->ring_virt + sizeof(struct vring_desc) * queue_size);
    q->used  = (struct vring_used*)(q->ring_virt + used_off);

    mmio_w32(VIRTIO_MMIO_QUEUE_NUM, queue_size);
    mmio_w32(VIRTIO_MMIO_QUEUE_ALIGN, VIRTQ_ALIGN);
    mmio_w32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(ring_pa / VNET_PAGE_SIZE));
    return 0;
}

static void vnet_kick(uint32_t queue_idx) {
    mmio_w32(VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
}

int aarch64_vnetmmio_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint32_t count = b->virtio_mmio_count ? b->virtio_mmio_count : 32;
    uint64_t tx_pa;

    if (g_ready) return 0;
    spinlock_init(&g_vnet_lock);

    if (b->first_virtio_mmio_base == 0) return -1;

    g_base = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t pa = b->first_virtio_mmio_base + (uint64_t)i * b->virtio_mmio_stride;
        volatile uint8_t* base = (volatile uint8_t*)(uintptr_t)aarch64_phys_to_virt(pa);

        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) continue;
        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_VERSION) != 1) continue;
        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_MMIO_DEVICE_ID_NET) continue;

        g_base = base;
        g_base_pa = pa;
        g_slot = i;
        break;
    }
    if (!g_base) return -1;

    mmio_w32(VIRTIO_MMIO_STATUS, 0);
    mmio_w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    (void)mmio_r32(VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    mmio_w32(VIRTIO_MMIO_GUEST_PAGE_SIZE, VNET_PAGE_SIZE);

    if (vnet_queue_setup(VIRTIO_RX_QUEUE, &g_rxq, VNET_QUEUE_MAX) != 0) return -1;
    if (vnet_queue_setup(VIRTIO_TX_QUEUE, &g_txq, VNET_QUEUE_MAX) != 0) return -1;

    g_rxq.active_descs = (g_rxq.queue_size > VIRTIO_RX_SLOTS) ? VIRTIO_RX_SLOTS : g_rxq.queue_size;
    g_txq.active_descs = 1;
    if (g_rxq.active_descs == 0) return -1;

    for (uint16_t i = 0; i < g_rxq.active_descs; i++) {
        uint64_t buf_pa = aarch64_pmm_alloc(1);
        if (!buf_pa) return -1;
        g_rx_buf_phys[i] = buf_pa;
        g_rx_bufs[i] = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(buf_pa);
        vnet_memset(g_rx_bufs[i], 0, VNET_PAGE_SIZE);

        g_rxq.desc[i].addr = g_rx_buf_phys[i];
        g_rxq.desc[i].len = sizeof(struct virtio_net_hdr) + VIRTIO_RX_BUF_SIZE;
        g_rxq.desc[i].flags = VRING_DESC_F_WRITE;
        g_rxq.desc[i].next = 0;
        g_rxq.avail->ring[i] = i;
    }
    g_rxq.avail->idx = g_rxq.active_descs;

    tx_pa = aarch64_pmm_alloc(1);
    if (!tx_pa) return -1;
    g_tx_buf_phys = tx_pa;
    g_tx_buf = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(tx_pa);
    vnet_memset(g_tx_buf, 0, VNET_PAGE_SIZE);
    g_txq.desc[VIRTIO_TX_SLOT].addr = g_tx_buf_phys;
    g_txq.desc[VIRTIO_TX_SLOT].len = 0;
    g_txq.desc[VIRTIO_TX_SLOT].flags = 0;
    g_txq.desc[VIRTIO_TX_SLOT].next = 0;

    /* MAC は config 空間の先頭 6 バイト (legacy、feature 交渉なしでも読める) */
    for (uint16_t i = 0; i < 6; i++) {
        g_mac[i] = *(volatile uint8_t*)(g_base + VIRTIO_MMIO_CONFIG + i);
    }

    vnet_kick(VIRTIO_RX_QUEUE);
    mmio_w32(VIRTIO_MMIO_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_ready = 1;

    put("[net] virtio-net-mmio ready slot=");
    putdec(g_slot);
    put(" mac=");
    for (uint16_t i = 0; i < 6; i++) {
        puthex_bare(g_mac[i], 2);
        if (i + 1 != 6) put(":");
    }
    put("\r\n");
    return 0;
}

static void vnet_reclaim_tx(void) {
    while (g_txq.last_used_idx != g_txq.used->idx) {
        g_txq.last_used_idx++;
        g_tx_busy = 0;
    }
}

/* **コールバックはロックの外で呼ぶ。**lwIP の DHCP は OFFER を受け取った
 * その場 (このコールバックの呼び出しスタックの中) で同期的に REQUEST を
 * 送り返す。ロックを持ったまま g_rx_cb() を呼ぶと、その送信が
 * virtio_net_send() -> 同じ g_vnet_lock で自己デッドロックする
 * (2026-09-04、実機ではなく QEMU virtio-net-device への DHCP で実際に踏んだ。
 * 1 個目の OFFER を受けたところで完全に止まり、以後 1 行も出なくなった —
 * timer 割り込みの中で spin していたので tick も全部止まっていた)。
 * **リング操作 (使用済みの取り出し・空きへの返却) だけをロックで守り、
 * コールバックはロックを外してから呼ぶ。** */
void aarch64_vnetmmio_poll(void) {
    uint16_t ids[VIRTIO_RX_SLOTS];
    uint32_t lens[VIRTIO_RX_SLOTS];
    uint16_t n = 0;
    uint16_t i;

    if (!g_ready) return;

    spin_lock(&g_vnet_lock);
    vnet_reclaim_tx();
    while (g_rxq.last_used_idx != g_rxq.used->idx && n < VIRTIO_RX_SLOTS) {
        struct vring_used_elem* elem = &g_rxq.used->ring[g_rxq.last_used_idx % g_rxq.queue_size];
        ids[n] = (uint16_t)elem->id;
        lens[n] = elem->len;
        n++;
        g_rxq.last_used_idx++;
    }
    spin_unlock(&g_vnet_lock);

    for (i = 0; i < n; i++) {
        uint16_t desc_id = ids[i];
        uint32_t total_len = lens[i];
        uint16_t frame_len;

        if (desc_id < g_rxq.active_descs && total_len > sizeof(struct virtio_net_hdr)) {
            frame_len = (uint16_t)(total_len - sizeof(struct virtio_net_hdr));
            if (frame_len > VIRTIO_RX_BUF_SIZE) frame_len = VIRTIO_RX_BUF_SIZE;
            /* **バッファの中身は空きに返すまで生きている。**avail へ戻すのは
             * この後 (ロックを取り直してから) なので、デバイスがまだ
             * 上書きしない */
            if (g_rx_cb && frame_len > 0) {
                g_rx_cb(g_rx_bufs[desc_id] + sizeof(struct virtio_net_hdr), frame_len);
            }
        }
    }

    if (n) {
        spin_lock(&g_vnet_lock);
        for (i = 0; i < n; i++) {
            g_rxq.avail->ring[g_rxq.avail->idx % g_rxq.queue_size] = ids[i];
            g_rxq.avail->idx++;
        }
        __sync_synchronize();
        vnet_kick(VIRTIO_RX_QUEUE);
        spin_unlock(&g_vnet_lock);
    }

    mmio_w32(VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(VIRTIO_MMIO_INTERRUPT_STATUS));
}

/* **常にポーリング。**冒頭のコメントのとおり、割り込み経路はまだ無い */
int aarch64_vnetmmio_needs_poll_fallback(void) {
    return g_ready;
}

int aarch64_vnetmmio_is_ready(void) {
    return g_ready;
}

int aarch64_vnetmmio_send(const void* frame, uint16_t len) {
    struct virtio_net_hdr* hdr;
    uint8_t* payload;
    const uint8_t* src = (const uint8_t*)frame;

    if (!g_ready || !frame || len == 0 || len > VIRTIO_FRAME_MAX) return -1;

    spin_lock(&g_vnet_lock);
    vnet_reclaim_tx();
    if (g_tx_busy) { spin_unlock(&g_vnet_lock); return -1; }

    hdr = (struct virtio_net_hdr*)g_tx_buf;
    payload = g_tx_buf + sizeof(struct virtio_net_hdr);
    vnet_memset(hdr, 0, sizeof(*hdr));
    for (uint16_t i = 0; i < len; i++) payload[i] = src[i];

    g_txq.desc[VIRTIO_TX_SLOT].len = (uint32_t)(sizeof(struct virtio_net_hdr) + len);
    g_txq.avail->ring[g_txq.avail->idx % g_txq.queue_size] = VIRTIO_TX_SLOT;
    __sync_synchronize();
    g_txq.avail->idx++;
    __sync_synchronize();
    g_tx_busy = 1;
    vnet_kick(VIRTIO_TX_QUEUE);
    spin_unlock(&g_vnet_lock);
    return 0;
}

const uint8_t* aarch64_vnetmmio_mac(void) {
    return g_mac;
}

void aarch64_vnetmmio_set_rx_callback(virtio_net_rx_cb_t cb) {
    g_rx_cb = cb;
}
