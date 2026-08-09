/*
 * virtio-mmio 版 virtio-blk (M4-1)。**ポーリング方式。**
 *
 * kernel/riscv64/virtio_blk_mmio.c (318 行) を土台にしている。QEMU virt の
 * virtio-mmio は riscv64 と同じ規格なので、**vring もリクエスト形式も
 * 共有ヘッダ (include/virtio.h / virtio_blk.h) をそのまま使える。**
 * 引き継ぎに「ほぼ流用できる」と書いてあったとおりだった。
 *
 * aarch64 で変えたのは 3 点:
 *
 *   1. **MMIO は phys_to_virt 経由。** カーネルが上位 VA に居るので、
 *      DTB 由来の物理アドレスを直に触れない
 *   2. **vring は aarch64_pmm_alloc から取る。** デバイスには物理アドレスを
 *      渡し、こちらは上位 VA で触る
 *   3. **spinlock / task / plic に依存しない。** SMP は 1 本、待ちは
 *      ポーリングなので要らない。割り込みでの完了通知は M4-2
 *
 * ★ 実機で効く注意点:
 *
 *   vring は**カーネルが Normal (キャッシュ有効) で張った領域を、
 *   デバイスが DMA で読み書きする**。QEMU はキャッシュを模していないので
 *   そのまま通るが、**Raspberry Pi 4 ではコヒーレンシの手当てが要る**
 *   (非キャッシュで張るか、キャッシュ操作を挟む)。
 *   M2 の「デバイスを Normal で張ると実機で壊れる」と同じ形で、
 *   **QEMU のスモークでは検出できない。**
 */
#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "virtio_blk.h"
#include "aarch64/boot.h"
#include "aarch64/vm.h"

uint64_t aarch64_pmm_alloc(uint64_t pages);

/* virtio-mmio レジスタ (virtio spec 4.2.2、legacy は 4.2.4) */
#define VIRTIO_MMIO_MAGIC_VALUE      0x000
#define VIRTIO_MMIO_VERSION          0x004
#define VIRTIO_MMIO_DEVICE_ID        0x008
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE  0x028  /* legacy のみ */
#define VIRTIO_MMIO_QUEUE_SEL        0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_MMIO_QUEUE_NUM        0x038
#define VIRTIO_MMIO_QUEUE_ALIGN      0x03c  /* legacy のみ */
#define VIRTIO_MMIO_QUEUE_PFN        0x040  /* legacy のみ */
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS           0x070
#define VIRTIO_MMIO_CONFIG           0x100

#define VIRTIO_MMIO_MAGIC            0x74726976U   /* "virt" */
#define VIRTIO_MMIO_DEVICE_ID_BLK    2

/* ステータスビットは include/virtio.h が持っている。ここで再定義しない
 * (VIRTIO_STATUS_DRIVER / DRIVER_OK が衝突する) */
#ifndef VIRTIO_STATUS_ACK
#define VIRTIO_STATUS_ACK       1
#endif

#define VBLK_QUEUE_MAX   128
#define VBLK_SECTOR_SIZE 512U
#define VBLK_PAGE_SIZE   4096U

static volatile uint8_t* g_base;        /* 上位 VA */
static uint64_t g_base_pa;
static struct virtio_queue g_q;
static struct virtio_blk_req* g_hdr;    /* 上位 VA */
static uint64_t g_hdr_pa;
static uint8_t* g_status;
static uint64_t g_capacity;             /* セクタ数 */

static void mmio_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_base + off) = v;
}
static uint32_t mmio_r32(uint32_t off) {
    return *(volatile uint32_t*)(g_base + off);
}

static void vblk_memset(void* p, uint8_t v, uint64_t n) {
    uint8_t* d = (uint8_t*)p;
    while (n--) *d++ = v;
}

uint64_t aarch64_virtio_blk_capacity(void) { return g_capacity; }
int aarch64_virtio_blk_present(void) { return g_base != 0; }
uint64_t aarch64_virtio_blk_base_pa(void) { return g_base_pa; }

/* legacy のキュー設定。**リングの物理アドレスをページ番号で渡す** */
static int vblk_queue_setup(void) {
    uint32_t queue_max, queue_size, avail_bytes, used_off, used_bytes, bytes, pages;
    uint64_t ring_pa;

    mmio_w32(VIRTIO_MMIO_QUEUE_SEL, 0);
    queue_max = mmio_r32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max == 0) return -1;
    queue_size = queue_max < VBLK_QUEUE_MAX ? queue_max : VBLK_QUEUE_MAX;

    avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + queue_size));
    used_off = (uint32_t)((sizeof(struct vring_desc) * queue_size + avail_bytes +
                           (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    used_bytes = (uint32_t)(sizeof(uint16_t) * 3U +
                            sizeof(struct vring_used_elem) * queue_size);
    bytes = used_off + used_bytes;
    pages = (bytes + VBLK_PAGE_SIZE - 1) / VBLK_PAGE_SIZE;

    ring_pa = aarch64_pmm_alloc(pages);
    if (!ring_pa) return -1;

    g_q.queue_size = (uint16_t)queue_size;
    g_q.ring_phys = ring_pa;
    g_q.ring_virt = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(ring_pa);
    g_q.active_descs = 0;
    g_q.last_used_idx = 0;
    vblk_memset(g_q.ring_virt, 0, (uint64_t)pages * VBLK_PAGE_SIZE);
    g_q.desc  = (struct vring_desc*)g_q.ring_virt;
    g_q.avail = (struct vring_avail*)(g_q.ring_virt + sizeof(struct vring_desc) * queue_size);
    g_q.used  = (struct vring_used*)(g_q.ring_virt + used_off);

    mmio_w32(VIRTIO_MMIO_QUEUE_NUM, queue_size);
    mmio_w32(VIRTIO_MMIO_QUEUE_ALIGN, VIRTQ_ALIGN);
    /* **物理アドレスをページ番号で渡す。** VA を渡すとデバイスが
     * まったく別の場所を読みに行く */
    mmio_w32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(ring_pa / VBLK_PAGE_SIZE));
    return 0;
}

/* 1 リクエスト = ヘッダ + データ + ステータスの 3 ディスクリプタ。
 * **ディスクリプタは 1 組しか持たない**ので、1 件ずつ。SMP と割り込み
 * 完了を入れるときはロックが要る (riscv64 版の教訓) */
static int vblk_rw(uint32_t type, uint64_t sector, void* buf, uint32_t sectors) {
    struct virtio_queue* q = &g_q;
    uint16_t used_idx;
    uint64_t spin = 0;

    if (!g_base || !buf || sectors == 0) return -1;

    g_hdr->type = type;
    g_hdr->reserved = 0;
    g_hdr->sector = sector;
    *g_status = 0xFF;

    q->desc[0].addr = g_hdr_pa;
    q->desc[0].len = sizeof(struct virtio_blk_req);
    q->desc[0].flags = VRING_DESC_F_NEXT;
    q->desc[0].next = 1;

    /* **デバイスに渡すのは物理アドレス。** カーネルの VA をそのまま
     * 書くと、デバイスは上位 VA を物理として読みに行って何も起きない */
    q->desc[1].addr = aarch64_virt_to_phys((uint64_t)(uintptr_t)buf);
    q->desc[1].len = sectors * VBLK_SECTOR_SIZE;
    q->desc[1].flags = (uint16_t)(VRING_DESC_F_NEXT |
                        (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0));
    q->desc[1].next = 2;

    q->desc[2].addr = g_hdr_pa + sizeof(struct virtio_blk_req);
    q->desc[2].len = 1;
    q->desc[2].flags = VRING_DESC_F_WRITE;
    q->desc[2].next = 0;

    used_idx = q->used->idx;
    q->avail->ring[q->avail->idx % q->queue_size] = 0;
    __sync_synchronize();
    q->avail->idx++;
    __sync_synchronize();
    mmio_w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* ポーリングで待つ。**無限には待たない。** デバイスが応答しないときに
     * 沈黙するのではなく、失敗として返せるようにする */
    while (q->used->idx == used_idx) {
        __sync_synchronize();
        if (++spin > 100000000ULL) return -2;   /* 応答なし */
    }
    q->last_used_idx = q->used->idx;

    /* 割り込みは使っていないが、状態は片づけておく */
    mmio_w32(VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(VIRTIO_MMIO_INTERRUPT_STATUS));

    return (*g_status == VIRTIO_BLK_S_OK) ? 0 : -1;
}

int aarch64_virtio_blk_read(uint64_t lba, void* buf, uint32_t sectors) {
    return vblk_rw(VIRTIO_BLK_T_IN, lba, buf, sectors);
}

int aarch64_virtio_blk_write(uint64_t lba, const void* buf, uint32_t sectors) {
    return vblk_rw(VIRTIO_BLK_T_OUT, lba, (void*)buf, sectors);
}

/* スロットを走査して virtio-blk を見つける。
 * **アドレスと本数と刻み幅は DTB 由来** (M2b)。直書きしない */
int aarch64_virtio_blk_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint64_t hdr_pa;
    uint32_t count = b->virtio_mmio_count ? b->virtio_mmio_count : 32;

    g_base = 0;
    g_base_pa = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t pa = b->first_virtio_mmio_base + (uint64_t)i * b->virtio_mmio_stride;
        volatile uint8_t* base = (volatile uint8_t*)(uintptr_t)aarch64_phys_to_virt(pa);

        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) continue;
        /* **legacy (version 1) のみ対応。** riscv64 版と同じ。QEMU の
         * virtio-mmio は既定が legacy なのでこれで足りる */
        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_VERSION) != 1) continue;
        if (*(volatile uint32_t*)(base + VIRTIO_MMIO_DEVICE_ID) != VIRTIO_MMIO_DEVICE_ID_BLK) continue;

        g_base = base;
        g_base_pa = pa;
        break;
    }
    if (!g_base) return -1;

    /* legacy の初期化: RESET → ACK → DRIVER → features → queue → DRIVER_OK */
    mmio_w32(VIRTIO_MMIO_STATUS, 0);
    mmio_w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    mmio_w32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 機能は何も受け取らない。**読むだけ読んで 0 を返す**のが legacy の作法 */
    (void)mmio_r32(VIRTIO_MMIO_DEVICE_FEATURES);
    mmio_w32(VIRTIO_MMIO_DRIVER_FEATURES, 0);

    mmio_w32(VIRTIO_MMIO_GUEST_PAGE_SIZE, VBLK_PAGE_SIZE);

    /* ヘッダとステータスを 1 ページに置く。デバイスが読むので物理も持つ */
    hdr_pa = aarch64_pmm_alloc(1);
    if (!hdr_pa) return -1;
    g_hdr_pa = hdr_pa;
    g_hdr = (struct virtio_blk_req*)(uintptr_t)aarch64_phys_to_virt(hdr_pa);
    g_status = (uint8_t*)(uintptr_t)(aarch64_phys_to_virt(hdr_pa) +
                                     sizeof(struct virtio_blk_req));

    if (vblk_queue_setup() != 0) return -1;

    mmio_w32(VIRTIO_MMIO_STATUS,
             VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* 容量は config 空間の先頭 (セクタ数、64bit) */
    g_capacity = (uint64_t)mmio_r32(VIRTIO_MMIO_CONFIG) |
                 ((uint64_t)mmio_r32(VIRTIO_MMIO_CONFIG + 4) << 32);
    return 0;
}

/* ---- 自己診断 (M4-1) ------------------------------------------------------
 *
 * **「見つかった」だけでは何の証拠にもならない。** 実際に読んで、
 * 書いたものが読み戻せるところまで見る。
 *
 * 判定は 3 本立て:
 *   1. LBA 0 に仕込んだ既知の文字列が読めること (読みが本当に効いている)
 *   2. LBA 1 が LBA 0 と違うこと (**同じ場所を読んでいないこと**の確認。
 *      LBA を無視していると 2 回とも同じ中身が返る)
 *   3. 書いて読み戻せること (書きが効いている)
 */
static uint8_t g_probe_buf[VBLK_SECTOR_SIZE] __attribute__((aligned(16)));
static uint8_t g_probe_buf2[VBLK_SECTOR_SIZE] __attribute__((aligned(16)));

static int vblk_str_eq_n(const uint8_t* a, const char* b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) if (a[i] != (uint8_t)b[i]) return 0;
    return 1;
}

static void vblk_put_ascii(const uint8_t* p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        char c = (char)p[i];
        aarch64_uart_putchar((c >= 0x20 && c < 0x7f) ? c : '.');
    }
}

void aarch64_virtio_blk_selftest(void) {
    int rc;
    /* **判定は 1 つの変数に集める。** 逆確認で、lba1 の結果を見ずに
     * aarch64-virtio-ok を出していたことが分かった (LBA を無視するよう
     * 壊しても ok が出た)。個々の行に ok/BAD を書くだけでは、
     * 最終判定に反映され忘れる */
    int ok = 1;

    aarch64_uart_puts("--- M4: virtio-mmio (virtio-blk) ---\n");

    if (aarch64_virtio_blk_init() != 0) {
        aarch64_uart_puts("  device    : 見つからない (-drive / -device を付けずに起動した)\n");
        aarch64_uart_puts("aarch64-virtio-none\n");
        return;
    }

    aarch64_uart_puts("  device    : ");
    aarch64_uart_puthex64(aarch64_virtio_blk_base_pa());
    aarch64_uart_puts("  (DTB 由来のスロットを走査して発見)\n  capacity  : ");
    aarch64_uart_puthex64(aarch64_virtio_blk_capacity());
    aarch64_uart_puts(" セクタ\n");

    /* --- 1. LBA 0 を読む --- */
    rc = aarch64_virtio_blk_read(0, g_probe_buf, 1);
    aarch64_uart_puts("  read lba0 : ");
    if (rc != 0) {
        aarch64_uart_puts("失敗 rc=");
        aarch64_uart_puthex64((uint64_t)(int64_t)rc);
        aarch64_uart_puts("\naarch64-virtio-BAD\n");
        return;
    }
    aarch64_uart_puts("\"");
    vblk_put_ascii(g_probe_buf, 24);
    aarch64_uart_puts("\"");
    if (vblk_str_eq_n(g_probe_buf, "ORTHOX-AARCH64-M4-SEC000", 24)) {
        aarch64_uart_puts("  ok\n");
    } else {
        aarch64_uart_puts("  BAD (期待した中身でない)\n");
        ok = 0;
    }

    /* --- 2. LBA 1 を読む。**LBA 0 と違うこと** --- */
    rc = aarch64_virtio_blk_read(1, g_probe_buf2, 1);
    aarch64_uart_puts("  read lba1 : ");
    if (rc != 0) {
        aarch64_uart_puts("失敗\naarch64-virtio-BAD\n");
        return;
    }
    aarch64_uart_puts("\"");
    vblk_put_ascii(g_probe_buf2, 24);
    aarch64_uart_puts("\"");
    if (vblk_str_eq_n(g_probe_buf2, "ORTHOX-AARCH64-M4-SEC001", 24)) {
        aarch64_uart_puts("  ok (LBA が効いている)\n");
    } else {
        aarch64_uart_puts("  BAD (LBA が効いていない)\n");
        ok = 0;
    }

    /* --- 3. 書いて読み戻す --- */
    for (unsigned i = 0; i < VBLK_SECTOR_SIZE; i++) g_probe_buf[i] = (uint8_t)(i & 0xff);
    g_probe_buf[0] = 'W'; g_probe_buf[1] = 'R'; g_probe_buf[2] = '!';
    rc = aarch64_virtio_blk_write(4, g_probe_buf, 1);
    if (rc == 0) {
        vblk_memset(g_probe_buf2, 0, VBLK_SECTOR_SIZE);
        rc = aarch64_virtio_blk_read(4, g_probe_buf2, 1);
    }
    aarch64_uart_puts("  write/read: ");
    if (rc != 0) {
        aarch64_uart_puts("失敗\naarch64-virtio-BAD\n");
        return;
    }
    {
        int same = 1;
        for (unsigned i = 0; i < VBLK_SECTOR_SIZE; i++) {
            if (g_probe_buf[i] != g_probe_buf2[i]) { same = 0; break; }
        }
        aarch64_uart_puts(same ? "ok (書いたものが読み戻せた)\n"
                               : "BAD (書き戻しが一致しない)\n");
        if (!same) ok = 0;
    }

    aarch64_uart_puts(ok ? "aarch64-virtio-ok\n" : "aarch64-virtio-BAD\n");
}
