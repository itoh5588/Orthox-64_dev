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
 *   3. **task / plic に依存しない。** 待ちはポーリング (または完了割り込み)
 *      なので要らない。割り込みでの完了通知は M4-2。
 *      **spinlock だけは要る** — 2026-08-25 に SMP で踏んだ (下の g_vblk_lock)
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
#include "spinlock.h"
#include "aarch64/boot.h"
#include "aarch64/vm.h"

uint64_t aarch64_pmm_alloc(uint64_t pages);
void aarch64_gic_enable_irq(unsigned intid);
uint64_t aarch64_timer_ticks(void);

/* 割り込みを待つ上限。**時間は tick で測る。**
 * wfi で寝ると「回した回数」は割り込みが来た回数にしかならないので、
 * 時間の目安にならない (逆確認で、上限 100 万回が実質 3 時間になった) */
#define VBLK_IRQ_TIMEOUT_TICKS 300   /* 10ms x 300 = 3 秒 */

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
static uint32_t g_intid;                /* 完了割り込みの INTID。0 = ポーリング */
static uint32_t g_slot;                 /* 見つけたスロット番号 */
static volatile uint32_t g_irq_done;    /* 割り込みが上がった印 */
static uint64_t g_irq_count;            /* 割り込みで完了した回数 */

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
uint32_t aarch64_virtio_blk_intid(void) { return g_intid; }
uint64_t aarch64_virtio_blk_irq_count(void) { return g_irq_count; }

int aarch64_virtio_blk_read(uint64_t lba, void* buf, uint32_t sectors);
int aarch64_virtio_blk_write(uint64_t lba, const void* buf, uint32_t sectors);

/* ---- storage 層の受け口 (M4-3) ------------------------------------------
 *
 * **単位が違う。** storage 層は「ブロック」を数え、virtio-blk は 512 バイト
 * のセクタを数える。xv6fs は BSIZE=1024 なので、storage_register_device に
 * 512 を渡してブロック = セクタに揃えてある (xv6bio が 2 セクタずつ読む)。
 *
 * **戻り値は「成功なら 0」。** ブロック数ではない (xv6bio が ret != 0 を
 * エラーとして扱う)。count を返すと、読めているのに毎回エラーログが出て
 * マウントが静かに失敗する。riscv64 の受け口も 0 を返している */
int aarch64_virtio_blk_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    (void)ctx;
    return aarch64_virtio_blk_read(lba, buf, (uint32_t)count);
}

int aarch64_virtio_blk_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    (void)ctx;
    return aarch64_virtio_blk_write(lba, buf, (uint32_t)count);
}

/* 完了割り込み。**ここで印を立てるだけ。** used->idx はデバイスが直接
 * 書いているので、待ち手に「見に行ってよい」と伝えれば足りる */
void aarch64_virtio_blk_irq(void) {
    if (!g_base) return;
    mmio_w32(VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(VIRTIO_MMIO_INTERRUPT_STATUS));
    g_irq_count++;
    g_irq_done = 1;
}

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

/* ---- 1 件ずつに直列化する (2026-08-25, M-1) -------------------------------
 *
 * **ディスクリプタは 1 組しか持たない。** `g_q.desc[0..2]` も `g_hdr` も
 * `g_status` も `g_irq_done` もグローバルに 1 つずつで、この関数は
 * それを毎回上書きする。**1 コア前提の作りのまま SMP に乗っていた。**
 *
 * 2 つの CPU が同時に入ると、こうなる (実測で踏んだ形):
 *
 *   CPU A: desc[1].addr = A のバッファ / avail->idx++ / notify
 *   CPU B: **デバイスが desc を読む前に** desc[1].addr = B のバッファ
 *   デバイス: **A のブロックを B のバッファへ DMA する**
 *
 * `used_idx` の控えも `*g_status` も `g_irq_done` も同じようにぶつかる。
 *
 * 症状は「ディスクが壊れる」ではなく **「読んだ中身が別のブロックになる」**。
 * 2026-08-25 の実測では、exec したばかりの busybox のテキストページが
 * 別物になり、EL0 が
 *
 *   ESR 0x92000006 (データアボート)  ELR 0x401c24  FAR 0x9b
 *
 * で落ちた。**ELR の命令は `b` (無条件分岐) でデータアボートを起こしよう
 * がない** — そこがページの中身が違うことの証拠になった。
 * 4 コアで 12 回中 4 回。1 コアでは 0 回。
 *
 * **IRQ は閉じない。** 完了待ちが `wfi` で `g_irq_done` を待つので、
 * 閉じると完了割り込みが来ずに固まる。EL1 の途中ではプリエンプトしない
 * (aarch64_task_resched_if_needed が SPSR を見て弾く) ので、素の
 * spin_lock で足りる。**この関数は割り込みハンドラからは呼ばれない**
 * (ハンドラは g_irq_done を立てるだけ) ので再入もしない。 */
static spinlock_t g_vblk_lock;

static int vblk_rw_locked(uint32_t type, uint64_t sector, void* buf, uint32_t sectors) {
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
    g_irq_done = 0;
    q->avail->ring[q->avail->idx % q->queue_size] = 0;
    __sync_synchronize();
    q->avail->idx++;
    __sync_synchronize();
    mmio_w32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    if (g_intid) {
        /* **割り込みの印だけで抜ける。** used->idx のポーリングを併用すると、
         * 割り込みが来なくても通ってしまい「割り込みで完了している」証拠に
         * ならない。来なければ時間切れで失敗として返す */
        uint64_t deadline = aarch64_timer_ticks() + VBLK_IRQ_TIMEOUT_TICKS;
        while (!g_irq_done) {
            __asm__ volatile("wfi");
            if (aarch64_timer_ticks() > deadline) return -2;   /* 割り込みが来ない */
        }
        (void)spin;
    } else {
        /* 割り込み番号が確かめられなかったときの退避経路 */
        while (q->used->idx == used_idx) {
            __sync_synchronize();
            if (++spin > 100000000ULL) return -2;
        }
        mmio_w32(VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(VIRTIO_MMIO_INTERRUPT_STATUS));
    }
    __sync_synchronize();
    q->last_used_idx = q->used->idx;

    return (*g_status == VIRTIO_BLK_S_OK) ? 0 : -1;
}

static int vblk_rw(uint32_t type, uint64_t sector, void* buf, uint32_t sectors) {
    int ret;
    spin_lock(&g_vblk_lock);
    ret = vblk_rw_locked(type, sector, buf, sectors);
    spin_unlock(&g_vblk_lock);
    return ret;
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

    /* static は 0 なので実質不要だが、**ロックの持ち主をここで明示する** */
    spinlock_init(&g_vblk_lock);

    g_base = 0;
    g_base_pa = 0;

    /* **番地 0 は「この機械には virtio が無い」の印** (boot.c)。
     * 張っていない VA を読みに行かせない。Pi 4 がこちら */
    if (b->first_virtio_mmio_base == 0) return -1;

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
        g_slot = i;
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

    /* **完了割り込みを有効にする。** スロット i の INTID = base + i が
     * DTB で確かめられているときだけ。確かめられていなければポーリングに
     * 退く (別のデバイスの割り込みを待つより、遅いほうがまし) */
    if (b->flags & AARCH64_BOOT_FLAG_VIRTIO_IRQ_OK) {
        g_intid = b->virtio_mmio_irq_base + g_slot;
        aarch64_gic_enable_irq(g_intid);
    }

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
        aarch64_uart_puts("  device    : not found (booted without -drive / -device)\n");
        aarch64_uart_puts("aarch64-virtio-none\n");
        return;
    }

    aarch64_uart_puts("  device    : ");
    aarch64_uart_puthex64(aarch64_virtio_blk_base_pa());
    aarch64_uart_puts("  (found by scanning DTB-derived slots)\n  capacity  : ");
    aarch64_uart_puthex64(aarch64_virtio_blk_capacity());
    aarch64_uart_puts(" sectors\n  intid     : ");
    aarch64_uart_puthex64(aarch64_virtio_blk_intid());
    aarch64_uart_puts(aarch64_virtio_blk_intid() ? "  (waiting for completion via interrupt)\n"
                                                 : "  (fell back to polling)\n");

    /* --- 1. LBA 0 を読む --- */
    rc = aarch64_virtio_blk_read(0, g_probe_buf, 1);
    aarch64_uart_puts("  read lba0 : ");
    if (rc != 0) {
        aarch64_uart_puts("failed rc=");
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
        aarch64_uart_puts("  BAD (not the expected content)\n");
        ok = 0;
    }

    /* --- 2. LBA 1 を読む。**LBA 0 と違うこと** --- */
    rc = aarch64_virtio_blk_read(1, g_probe_buf2, 1);
    aarch64_uart_puts("  read lba1 : ");
    if (rc != 0) {
        aarch64_uart_puts("failed\naarch64-virtio-BAD\n");
        return;
    }
    aarch64_uart_puts("\"");
    vblk_put_ascii(g_probe_buf2, 24);
    aarch64_uart_puts("\"");
    if (vblk_str_eq_n(g_probe_buf2, "ORTHOX-AARCH64-M4-SEC001", 24)) {
        aarch64_uart_puts("  ok (LBA is in effect)\n");
    } else {
        aarch64_uart_puts("  BAD (LBA not in effect)\n");
        ok = 0;
    }

    /* --- 3. 書いて読み戻す ---
     *
     * **書き先はディスクの最終セクタ。** M4-3 で同じディスクに xv6fs を
     * 載せたので、前のように LBA 4 へ書くとファイルシステムを壊す
     * (BSIZE=1024 なので LBA 4 は block 2 = ログ領域)。
     * スモークが作るイメージは、fs の後ろに余白を付けてある */
    {
        uint64_t probe_lba = g_capacity ? (g_capacity - 1) : 4;
        for (unsigned i = 0; i < VBLK_SECTOR_SIZE; i++) g_probe_buf[i] = (uint8_t)(i & 0xff);
        g_probe_buf[0] = 'W'; g_probe_buf[1] = 'R'; g_probe_buf[2] = '!';
        rc = aarch64_virtio_blk_write(probe_lba, g_probe_buf, 1);
        if (rc == 0) {
            vblk_memset(g_probe_buf2, 0, VBLK_SECTOR_SIZE);
            rc = aarch64_virtio_blk_read(probe_lba, g_probe_buf2, 1);
        }
    }
    aarch64_uart_puts("  write/read: ");
    if (rc != 0) {
        aarch64_uart_puts("failed\naarch64-virtio-BAD\n");
        return;
    }
    {
        int same = 1;
        for (unsigned i = 0; i < VBLK_SECTOR_SIZE; i++) {
            if (g_probe_buf[i] != g_probe_buf2[i]) { same = 0; break; }
        }
        aarch64_uart_puts(same ? "ok (what was written reads back)\n"
                               : "BAD (write-back mismatch)\n");
        if (!same) ok = 0;
    }

    /* --- 4. 割り込みで完了しているか -------------------------------------
     * **読み書きが通っただけでは、割り込みで完了した証拠にならない。**
     * ポーリングでも同じ結果になる。上で待ちを割り込みの印だけにしてある
     * ので、回数が 0 でなければ本当に割り込みで抜けている */
    aarch64_uart_puts("  irq count : ");
    aarch64_uart_puthex64(aarch64_virtio_blk_irq_count());
    if (aarch64_virtio_blk_intid() && aarch64_virtio_blk_irq_count() >= 4) {
        aarch64_uart_puts("  ok (completed via interrupt)\n");
    } else if (!aarch64_virtio_blk_intid()) {
        aarch64_uart_puts("  (0, since polling)\n");
    } else {
        aarch64_uart_puts("  BAD (interrupt not raised)\n");
        ok = 0;
    }

    aarch64_uart_puts(ok ? "aarch64-virtio-ok\n" : "aarch64-virtio-BAD\n");
}
