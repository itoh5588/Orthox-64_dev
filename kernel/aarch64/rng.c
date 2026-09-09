/* aarch64 の乱数源 (2026-09-05、TLS の手1)
 *
 * **なぜ要るか。** これまで aarch64 の getrandom(2) は
 * kernel/linux_syscall.c の xorshift で、種は
 *
 *     arch_time_now_ms() ^ (タスク構造体のアドレス) ^ 定数
 *
 * だった。**起動からの経過ミリ秒とカーネルの配置が分かれば再現できる。**
 * ふだんは実害が出ないが、TLS はここから鍵の材料を取る。予測できる乱数で
 * 握手すると、**TLS の形はしているが中身は守られていない**ものになる。
 *
 * **偽の乱数を返さない。**源が無ければ「無い」と答える (-ENOSYS)。
 * 呼んだ側が「乱数を得たつもり」で先へ進むのが一番悪い。
 *
 * 源は 2 つ:
 *
 *   実機 (Pi 4)  BCM2711 の RNG200 (DTB の brcm,bcm2711-rng200、0xfe104000)
 *   QEMU virt    virtio-rng (device id 4)。-device virtio-rng-device で付く
 *
 * どちらも無ければ源なし。**その場合に静かに劣化させない。**
 */
#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "spinlock.h"
#include "aarch64/boot.h"
#include "aarch64/vm.h"

uint64_t aarch64_pmm_alloc(uint64_t pages);
uint64_t aarch64_timer_ticks(void);


static spinlock_t g_rng_lock;

/* どの源を使っているか。0 = 無し */
#define RNG_SRC_NONE     0
#define RNG_SRC_RNG200   1
#define RNG_SRC_VIRTIO   2
static int g_src = RNG_SRC_NONE;

/* 取り出したバイト数の累計。**源が生きているかを後から見るため** */
static uint64_t g_bytes_out;

/* ==========================================================================
 * BCM2711 RNG200 (実機)
 *
 * レジスタの並びは Linux の drivers/char/hw_random/iproc-rng200.c と同じ。
 * **DTB の reg が 0x7e104000 長さ 0x28 で、下の最後のレジスタ (0x24) の
 * 直後で終わる** —— 並びが合っている裏付けになる。実機の Raspberry Pi OS
 * も "iproc-rng200 fe104000.rng: hwrng registered" を出しており、同じ
 * ドライバが同じ番地を掴んでいる。
 * ========================================================================== */
#define RNG200_CTRL              0x00
#define RNG200_SOFT_RESET        0x04
#define RBG200_SOFT_RESET        0x08
#define RNG200_INT_STATUS        0x18
#define RNG200_FIFO_DATA         0x20
#define RNG200_FIFO_COUNT        0x24

#define RNG200_CTRL_RBGEN_MASK       0x00001fffU
#define RNG200_CTRL_RBGEN_ENABLE     0x00000001U
#define RNG200_SOFT_RESET_BIT        0x00000001U
#define RBG200_SOFT_RESET_BIT        0x00000001U
/* 立つと FIFO が止まったままになるので、見つけたら初期化からやり直す */
#define RNG200_INT_MASTER_FAIL_LOCKOUT  0x80000000U
#define RNG200_FIFO_COUNT_MASK       0x000000ffU

static volatile uint8_t* g_rng200;      /* 上位 VA。0 なら未使用 */

static uint32_t rng200_r32(uint32_t off) {
    return *(volatile uint32_t*)(g_rng200 + off);
}
static void rng200_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_rng200 + off) = v;
}

static void rng200_reset_and_enable(void) {
    uint32_t v;

    /* いったん止める */
    v = rng200_r32(RNG200_CTRL) & ~RNG200_CTRL_RBGEN_MASK;
    rng200_w32(RNG200_CTRL, v);

    /* 割り込み状態を全部落とす */
    rng200_w32(RNG200_INT_STATUS, 0xffffffffU);

    /* RBG と RNG をリセット (立ててから寝かせる) */
    rng200_w32(RBG200_SOFT_RESET, rng200_r32(RBG200_SOFT_RESET) | RBG200_SOFT_RESET_BIT);
    rng200_w32(RNG200_SOFT_RESET, rng200_r32(RNG200_SOFT_RESET) | RNG200_SOFT_RESET_BIT);
    rng200_w32(RNG200_SOFT_RESET, rng200_r32(RNG200_SOFT_RESET) & ~RNG200_SOFT_RESET_BIT);
    rng200_w32(RBG200_SOFT_RESET, rng200_r32(RBG200_SOFT_RESET) & ~RBG200_SOFT_RESET_BIT);

    /* 動かす */
    v = (rng200_r32(RNG200_CTRL) & ~RNG200_CTRL_RBGEN_MASK) | RNG200_CTRL_RBGEN_ENABLE;
    rng200_w32(RNG200_CTRL, v);
}

/* FIFO から 32bit を 1 語取る。取れたら 1。
 * **待つ上限は tick で測る** (ループ回数だと機械の速さで意味が変わる) */
static int rng200_word(uint32_t* out) {
    uint64_t deadline = aarch64_timer_ticks() + 100U;   /* 10ms x 100 = 1 秒 */

    for (;;) {
        if (rng200_r32(RNG200_INT_STATUS) & RNG200_INT_MASTER_FAIL_LOCKOUT) {
            /* 締め出された。**黙って古い値を返さず、作り直す** */
            rng200_reset_and_enable();
        }
        if ((rng200_r32(RNG200_FIFO_COUNT) & RNG200_FIFO_COUNT_MASK) != 0U) {
            *out = rng200_r32(RNG200_FIFO_DATA);
            return 1;
        }
        if (aarch64_timer_ticks() > deadline) return 0;
        __asm__ volatile("yield");
    }
}

/* ==========================================================================
 * virtio-rng (QEMU virt)
 *
 * **一番単純な virtio デバイス。**キューは 1 本で、こちらが「書き込んで
 * よいバッファ」を 1 つ置くと、デバイスが乱数で埋めて返す。used の len が
 * 実際に埋めたバイト数になる。割り込みは使わず、used->idx を見て待つ
 * (取るのは握手のたびに 64 バイト程度で、頻度が低いため)。
 * ========================================================================== */
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

#define VIRTIO_MMIO_MAGIC            0x74726976U   /* "virt" */
#define VIRTIO_MMIO_DEVICE_ID_RNG    4
#ifndef VIRTIO_STATUS_ACK
#define VIRTIO_STATUS_ACK       1
#endif

#define VRNG_QUEUE_SIZE   8U
#define VRNG_PAGE_SIZE    4096U
#define VRNG_BUF_BYTES    256U

static volatile uint8_t* g_vrng;        /* 上位 VA。0 なら未使用 */
static struct virtio_queue g_vq;
static uint8_t* g_vbuf;                 /* デバイスが埋める領域 (上位 VA) */
static uint64_t g_vbuf_pa;

static uint32_t vrng_r32(uint32_t off) { return *(volatile uint32_t*)(g_vrng + off); }
static void vrng_w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(g_vrng + off) = v; }

static void rng_memset(void* p, uint8_t v, uint64_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) d[i] = v;
}

static int vrng_queue_setup(void) {
    uint32_t queue_max, queue_size, avail_bytes, used_off, used_bytes, bytes, pages;
    uint64_t ring_pa;

    vrng_w32(VIRTIO_MMIO_QUEUE_SEL, 0);
    queue_max = vrng_r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max == 0) return -1;
    queue_size = queue_max < VRNG_QUEUE_SIZE ? queue_max : VRNG_QUEUE_SIZE;

    avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + queue_size));
    used_off = (uint32_t)((sizeof(struct vring_desc) * queue_size + avail_bytes +
                           (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    used_bytes = (uint32_t)(sizeof(uint16_t) * 3U +
                            sizeof(struct vring_used_elem) * queue_size);
    bytes = used_off + used_bytes;
    pages = (bytes + VRNG_PAGE_SIZE - 1) / VRNG_PAGE_SIZE;

    /* **非キャッシュ枠から取る。**デバイスが DMA で書き換える領域を
     * キャッシュの効くページに置くと、実機では中身が見えない
     * (N-9 で GENET が踏んだのと同じ形。QEMU では通ってしまうので
     * スモークでは検出できない) */
    ring_pa = aarch64_vm_dma_alloc(pages);
    if (!ring_pa) return -1;

    g_vq.queue_size = (uint16_t)queue_size;
    g_vq.ring_phys = ring_pa;
    g_vq.ring_virt = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(ring_pa);
    g_vq.active_descs = 0;
    g_vq.last_used_idx = 0;
    rng_memset(g_vq.ring_virt, 0, (uint64_t)pages * VRNG_PAGE_SIZE);
    g_vq.desc  = (struct vring_desc*)g_vq.ring_virt;
    g_vq.avail = (struct vring_avail*)(g_vq.ring_virt + sizeof(struct vring_desc) * queue_size);
    g_vq.used  = (struct vring_used*)(g_vq.ring_virt + used_off);

    vrng_w32(VIRTIO_MMIO_QUEUE_NUM, queue_size);
    vrng_w32(VIRTIO_MMIO_QUEUE_ALIGN, VIRTQ_ALIGN);
    vrng_w32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(ring_pa / VRNG_PAGE_SIZE));
    return 0;
}

/* デバイスに 1 回埋めてもらう。埋まったバイト数を返す (0 なら取れなかった) */
static uint32_t vrng_fill(uint32_t want) {
    uint64_t deadline;
    uint16_t idx;
    uint32_t got;

    if (want > VRNG_BUF_BYTES) want = VRNG_BUF_BYTES;

    g_vq.desc[0].addr = g_vbuf_pa;
    g_vq.desc[0].len = want;
    g_vq.desc[0].flags = VRING_DESC_F_WRITE;   /* **デバイスが書く側** */
    g_vq.desc[0].next = 0;

    idx = g_vq.avail->idx;
    g_vq.avail->ring[idx % g_vq.queue_size] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    g_vq.avail->idx = (uint16_t)(idx + 1U);
    __asm__ volatile("dsb sy" ::: "memory");
    vrng_w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    deadline = aarch64_timer_ticks() + 100U;   /* 1 秒 */
    for (;;) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (g_vq.used->idx != g_vq.last_used_idx) break;
        if (aarch64_timer_ticks() > deadline) return 0;
        __asm__ volatile("yield");
    }
    got = g_vq.used->ring[g_vq.last_used_idx % g_vq.queue_size].len;
    g_vq.last_used_idx = (uint16_t)(g_vq.last_used_idx + 1U);
    vrng_w32(VIRTIO_MMIO_INTERRUPT_ACK, vrng_r32(VIRTIO_MMIO_INTERRUPT_STATUS));
    if (got > want) got = want;    /* デバイスの申告を鵜呑みにしない */
    return got;
}

static int vrng_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint32_t count = b->virtio_mmio_count ? b->virtio_mmio_count : 32;
    uint64_t buf_pa;

    if (b->first_virtio_mmio_base == 0) return -1;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t pa = b->first_virtio_mmio_base + (uint64_t)i * b->virtio_mmio_stride;
        volatile uint8_t* base = (volatile uint8_t*)(uintptr_t)aarch64_phys_to_virt(pa);

        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) continue;
        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_VERSION) != 1) continue;
        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_MMIO_DEVICE_ID_RNG) continue;
        g_vrng = base;
        break;
    }
    if (!g_vrng) return -1;

    vrng_w32(VIRTIO_MMIO_STATUS, 0);
    vrng_w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    vrng_w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    (void)vrng_r32(VIRTIO_MMIO_DEVICE_FEATURES);
    vrng_w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    vrng_w32(VIRTIO_MMIO_GUEST_PAGE_SIZE, VRNG_PAGE_SIZE);

    if (vrng_queue_setup() != 0) { g_vrng = 0; return -1; }

    buf_pa = aarch64_vm_dma_alloc(1);
    if (!buf_pa) { g_vrng = 0; return -1; }
    g_vbuf_pa = buf_pa;
    g_vbuf = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(buf_pa);
    rng_memset(g_vbuf, 0, VRNG_PAGE_SIZE);

    vrng_w32(VIRTIO_MMIO_STATUS,
             VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

/* ==========================================================================
 * 表に出る口
 * ========================================================================== */

/* **どちらの源も無ければ 0 を返し、以後 getrandom は -ENOSYS になる。**
 * 起動時に 1 行出す —— 「乱数源が無い」は黙って進んでよい話ではない */
int aarch64_rng_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();

    spinlock_init(&g_rng_lock);
    g_src = RNG_SRC_NONE;

    if (b->rng_base) {
        g_rng200 = (volatile uint8_t*)(uintptr_t)aarch64_phys_to_virt(b->rng_base);
        rng200_reset_and_enable();
        /* **本当に出るところまで見てから採用する。**番地が合っていても
         * 止まっていることはある (GENET の割り込みで学んだ形) */
        {
            uint32_t w;
            if (rng200_word(&w)) {
                g_src = RNG_SRC_RNG200;
                aarch64_uart_puts("rng: BCM2711 RNG200 ready\r\n");
                return 1;
            }
        }
        g_rng200 = 0;
        aarch64_uart_puts("rng: RNG200 found but produced nothing\r\n");
    }

    if (vrng_init() == 0) {
        uint32_t got = vrng_fill(8);
        if (got > 0) {
            g_src = RNG_SRC_VIRTIO;
            aarch64_uart_puts("rng: virtio-rng ready\r\n");
            return 1;
        }
        g_vrng = 0;
        aarch64_uart_puts("rng: virtio-rng found but produced nothing\r\n");
    }

    /* **ここを静かに通さない。**乱数を要る側 (TLS) が後で困る */
    aarch64_uart_puts("rng: no entropy source (getrandom will fail)\r\n");
    return 0;
}

int aarch64_rng_present(void) { return g_src != RNG_SRC_NONE; }
uint64_t aarch64_rng_bytes_served(void) { return g_bytes_out; }

/* buf を len バイト埋める。埋めた数を返す。源が無ければ -1。
 * **足りない分を作らない。**途中で取れなくなったらそこまでの数を返す */
int64_t arch_random_bytes(void* buf, size_t len) {
    uint8_t* out = (uint8_t*)buf;
    size_t off = 0;
    uint64_t flags;

    if (!out) return -1;
    if (g_src == RNG_SRC_NONE) return -1;

    flags = spin_lock_irqsave(&g_rng_lock);

    if (g_src == RNG_SRC_RNG200) {
        while (off < len) {
            uint32_t w;
            size_t take;
            if (!rng200_word(&w)) break;
            take = len - off;
            if (take > sizeof(w)) take = sizeof(w);
            for (size_t i = 0; i < take; i++) out[off + i] = (uint8_t)(w >> (i * 8));
            off += take;
        }
    } else {
        while (off < len) {
            uint32_t want = (uint32_t)(len - off);
            uint32_t got;
            if (want > VRNG_BUF_BYTES) want = VRNG_BUF_BYTES;
            got = vrng_fill(want);
            if (got == 0) break;
            for (uint32_t i = 0; i < got; i++) out[off + i] = g_vbuf[i];
            off += got;
        }
    }

    g_bytes_out += off;
    spin_unlock_irqrestore(&g_rng_lock, flags);
    return (int64_t)off;
}
