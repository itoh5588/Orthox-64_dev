// riscv64 virtio-mmio 版 virtio-blk ドライバ (ポーリング方式)
// QEMU virt の virtio-mmio スロットを走査して block デバイスを見つけ、
// storage 層に "vblk0" として登録する。x86 側 (virtio_blk.c, PCI legacy) とは
// トランスポートのみが異なり、vring / リクエスト形式は共通ヘッダを使う。

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "vmm.h"
#include "virtio.h"
#include "virtio_blk.h"
#include "riscv64/boot.h"
#include "spinlock.h"

// virtio-mmio レジスタ (virtio spec 4.2.2, legacy 4.2.4)
#define VIRTIO_MMIO_MAGIC_VALUE      0x000
#define VIRTIO_MMIO_VERSION          0x004
#define VIRTIO_MMIO_DEVICE_ID        0x008
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE  0x028  // legacy のみ
#define VIRTIO_MMIO_QUEUE_SEL        0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_MMIO_QUEUE_NUM        0x038
#define VIRTIO_MMIO_QUEUE_ALIGN      0x03c  // legacy のみ
#define VIRTIO_MMIO_QUEUE_PFN        0x040  // legacy のみ
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS           0x070
#define VIRTIO_MMIO_CONFIG           0x100

#define VIRTIO_MMIO_MAGIC            0x74726976U
#define VIRTIO_MMIO_DEVICE_ID_BLK    2

#define RISCV64_VIRTIO_MMIO_SLOT0    0x10001000ULL
#define RISCV64_VIRTIO_MMIO_STRIDE   0x1000ULL
#define RISCV64_VIRTIO_MMIO_SLOTS    8

#define VBLK_MMIO_QUEUE_MAX 128
#define VBLK_SECTOR_SIZE    512U

static volatile uint8_t* g_vblk_mmio_base;
static struct virtio_queue g_vblk_mmio_q;
static struct virtio_blk_req* g_vblk_mmio_hdr;
static uint8_t* g_vblk_mmio_status;
static uint64_t g_vblk_mmio_hdr_phys;
static uint64_t g_vblk_mmio_capacity;
/*
 * リクエスト 1 件分のディスクリプタ (desc[0..2]) とヘッダ/ステータス領域を
 * 1 組しか持たない設計なので、複数 hart が同時に入ると互いのリクエストを
 * 上書きしてしまう (SMP=4 で `xv6bio: disk read error` が多発した)。
 * 完了はポーリング待ちで yield しないため、リクエスト全体をスピンロックで
 * 囲って直列化する。静的変数のゼロ初期化 = spinlock_init 済み。
 */
static spinlock_t g_vblk_mmio_lock;

static inline uint32_t vblk_mmio_read32(uint32_t off) {
    return *(volatile uint32_t*)(g_vblk_mmio_base + off);
}

static inline void vblk_mmio_write32(uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(g_vblk_mmio_base + off) = value;
}

static void* vblk_memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static int vblk_mmio_setup_queue(void) {
    uint32_t queue_max;
    uint32_t queue_size;
    uint32_t avail_bytes;
    uint32_t used_off;
    uint32_t used_bytes;
    uint32_t bytes;
    size_t pages;
    void* ring_phys;

    vblk_mmio_write32(VIRTIO_MMIO_QUEUE_SEL, 0);
    queue_max = vblk_mmio_read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max == 0) return -1;
    queue_size = queue_max < VBLK_MMIO_QUEUE_MAX ? queue_max : VBLK_MMIO_QUEUE_MAX;

    avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + queue_size));
    used_off = (uint32_t)((sizeof(struct vring_desc) * queue_size + avail_bytes + (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    used_bytes = (uint32_t)(sizeof(uint16_t) * 3U + sizeof(struct vring_used_elem) * queue_size);
    bytes = used_off + used_bytes;
    pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    ring_phys = pmm_alloc(pages);
    if (!ring_phys) return -1;

    g_vblk_mmio_q.queue_size = (uint16_t)queue_size;
    g_vblk_mmio_q.ring_phys = (uint64_t)ring_phys;
    g_vblk_mmio_q.ring_virt = (uint8_t*)PHYS_TO_VIRT(ring_phys);
    g_vblk_mmio_q.active_descs = 0;
    g_vblk_mmio_q.last_used_idx = 0;
    vblk_memset(g_vblk_mmio_q.ring_virt, 0, pages * PAGE_SIZE);
    g_vblk_mmio_q.desc = (struct vring_desc*)g_vblk_mmio_q.ring_virt;
    g_vblk_mmio_q.avail = (struct vring_avail*)(g_vblk_mmio_q.ring_virt + sizeof(struct vring_desc) * queue_size);
    g_vblk_mmio_q.used = (struct vring_used*)(g_vblk_mmio_q.ring_virt + used_off);

    vblk_mmio_write32(VIRTIO_MMIO_QUEUE_NUM, queue_size);
    vblk_mmio_write32(VIRTIO_MMIO_QUEUE_ALIGN, VIRTQ_ALIGN);
    vblk_mmio_write32(VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(g_vblk_mmio_q.ring_phys / PAGE_SIZE));
    return 0;
}

// 1 リクエスト = ヘッダ + データ + ステータスの 3 ディスクリプタ (ポーリング完了待ち)
static int vblk_mmio_rw(uint32_t type, uint64_t sector, void* buf, uint32_t sectors) {
    struct virtio_queue* q = &g_vblk_mmio_q;
    uint16_t head = 0;
    uint16_t used_idx;
    uint64_t flags;
    int ret;

    if (!g_vblk_mmio_base || !buf || sectors == 0) return -1;

    flags = spin_lock_irqsave(&g_vblk_mmio_lock);
    g_vblk_mmio_hdr->type = type;
    g_vblk_mmio_hdr->reserved = 0;
    g_vblk_mmio_hdr->sector = sector;
    *g_vblk_mmio_status = 0xFF;

    q->desc[0].addr = g_vblk_mmio_hdr_phys;
    q->desc[0].len = sizeof(struct virtio_blk_req);
    q->desc[0].flags = VRING_DESC_F_NEXT;
    q->desc[0].next = 1;

    q->desc[1].addr = VIRT_TO_PHYS((uint64_t)buf);
    q->desc[1].len = sectors * VBLK_SECTOR_SIZE;
    q->desc[1].flags = (uint16_t)(VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0));
    q->desc[1].next = 2;

    q->desc[2].addr = g_vblk_mmio_hdr_phys + sizeof(struct virtio_blk_req);
    q->desc[2].len = 1;
    q->desc[2].flags = VRING_DESC_F_WRITE;
    q->desc[2].next = 0;

    used_idx = q->used->idx;
    q->avail->ring[q->avail->idx % q->queue_size] = head;
    __sync_synchronize();
    q->avail->idx++;
    __sync_synchronize();
    vblk_mmio_write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    while (q->used->idx == used_idx) {
        __sync_synchronize();
    }
    q->last_used_idx = q->used->idx;

    ret = (*g_vblk_mmio_status == VIRTIO_BLK_S_OK) ? 0 : -1;
    spin_unlock_irqrestore(&g_vblk_mmio_lock, flags);
    return ret;
}

static int vblk_mmio_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    (void)ctx;
    for (size_t i = 0; i < count; i++) {
        if (vblk_mmio_rw(VIRTIO_BLK_T_IN, lba + i, (uint8_t*)buf + i * VBLK_SECTOR_SIZE, 1) < 0) return -1;
    }
    return 0;
}

static int vblk_mmio_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    (void)ctx;
    for (size_t i = 0; i < count; i++) {
        if (vblk_mmio_rw(VIRTIO_BLK_T_OUT, lba + i, (uint8_t*)(uintptr_t)buf + i * VBLK_SECTOR_SIZE, 1) < 0) return -1;
    }
    return 0;
}

int riscv64_virtio_blk_mmio_init(void) {
    void* req_phys;
    uint32_t status;

    for (int slot = 0; slot < RISCV64_VIRTIO_MMIO_SLOTS; slot++) {
        volatile uint8_t* base = (volatile uint8_t*)(uintptr_t)(RISCV64_VIRTIO_MMIO_SLOT0 + (uint64_t)slot * RISCV64_VIRTIO_MMIO_STRIDE);
        g_vblk_mmio_base = base;
        if (vblk_mmio_read32(VIRTIO_MMIO_MAGIC_VALUE) != VIRTIO_MMIO_MAGIC) continue;
        if (vblk_mmio_read32(VIRTIO_MMIO_VERSION) != 1) continue;  // legacy のみ対応
        if (vblk_mmio_read32(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_MMIO_DEVICE_ID_BLK) continue;
        goto found;
    }
    g_vblk_mmio_base = 0;
    return -1;

found:
    // legacy 初期化: RESET → ACK → DRIVER → features → queue → DRIVER_OK
    vblk_mmio_write32(VIRTIO_MMIO_STATUS, 0);
    status = VIRTIO_STATUS_ACKNOWLEDGE;
    vblk_mmio_write32(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    vblk_mmio_write32(VIRTIO_MMIO_STATUS, status);

    (void)vblk_mmio_read32(VIRTIO_MMIO_DEVICE_FEATURES);
    vblk_mmio_write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    vblk_mmio_write32(VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);

    if (vblk_mmio_setup_queue() < 0) {
        vblk_mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        g_vblk_mmio_base = 0;
        return -1;
    }

    req_phys = pmm_alloc(1);
    if (!req_phys) {
        vblk_mmio_write32(VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        g_vblk_mmio_base = 0;
        return -1;
    }
    g_vblk_mmio_hdr_phys = (uint64_t)req_phys;
    g_vblk_mmio_hdr = (struct virtio_blk_req*)PHYS_TO_VIRT(req_phys);
    g_vblk_mmio_status = (uint8_t*)g_vblk_mmio_hdr + sizeof(struct virtio_blk_req);

    status |= VIRTIO_STATUS_DRIVER_OK;
    vblk_mmio_write32(VIRTIO_MMIO_STATUS, status);

    g_vblk_mmio_capacity = *(volatile uint64_t*)(g_vblk_mmio_base + VIRTIO_MMIO_CONFIG);
    return 0;
}

uint64_t riscv64_virtio_blk_mmio_capacity(void) {
    return g_vblk_mmio_capacity;
}

int riscv64_virtio_blk_mmio_present(void) {
    return g_vblk_mmio_base != 0;
}

int riscv64_virtio_blk_mmio_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    return vblk_mmio_storage_read(ctx, lba, buf, count);
}

int riscv64_virtio_blk_mmio_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    return vblk_mmio_storage_write(ctx, lba, buf, count);
}
