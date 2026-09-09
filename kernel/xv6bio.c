/*
 * Portions of this file are derived from xv6-riscv (MIT License):
 *   Copyright (c) 2006-2019 Frans Kaashoek, Robert Morris, Russ Cox,
 *                           Massachusetts Institute of Technology
 * The full xv6 MIT license text is reproduced in THIRD_PARTY_NOTICES.md.
 */
/*
 * xv6bio.c — バッファキャッシュ
 * xv6-riscv/kernel/bio.c を Orthox-64 向けに移植。
 *
 * 変更点:
 *   sleeplock → xv6_sleeplock (wait_queue ベース。I/O を跨ぐ b->lock で
 *               スピンするとデッドロックするため。xv6fs.h 参照)
 *   virtio_disk_rw → storage_read_blocks / storage_write_blocks
 *   NBUF 128 に増量
 */

#include "xv6fs.h"
#include "storage.h"
#include "kassert.h"
#include <stdarg.h>

extern int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap);
extern int64_t sys_write_serial(const char *buf, size_t count);

char g_xv6fs_devname[16];

static void xv6bio_log(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sys_write_serial(buf, (size_t)n);
}

/* ------------------------------------------------------------------ */

/* ディスク I/O ラッパー: BSIZE は 512 バイトセクター単位に変換 */
#define SECTORS_PER_BLOCK  (XV6FS_BSIZE / 512)

static void xv6fs_disk_rw(struct xv6buf *b, int write) {
    uint64_t lba = (uint64_t)b->blockno * SECTORS_PER_BLOCK;
    int ret;
    if (write) {
        ret = storage_write_blocks(g_xv6fs_devname, lba, b->data, SECTORS_PER_BLOCK);
    } else {
        ret = storage_read_blocks(g_xv6fs_devname, lba, b->data, SECTORS_PER_BLOCK);
    }
    if (ret != 0) {
        xv6bio_log("xv6bio: disk %s error: dev=%s block=%u ret=%d\n",
                   write ? "write" : "read", g_xv6fs_devname, b->blockno, ret);
    }
}

/* ------------------------------------------------------------------ */
/* 連続ブロックを 1 コマンドで運ぶ (P-5、2026-08-29)                    */
/*                                                                     */
/* xv6fs_disk_rw は 1 ブロックずつしか運ばない。1 回あたりの固定費      */
/* (読み 0.94 ms / 書き 2.27 ms) がブロック数だけ掛かるので、連続して   */
/* いる区間はまとめて 1 コマンドにする。**キャッシュは通さない。**      */
/* ------------------------------------------------------------------ */

int xv6bio_rw_run(uint32_t dev, uint32_t blockno, uint32_t nblocks,
                  void *buf, int write) {
    uint64_t lba = (uint64_t)blockno * SECTORS_PER_BLOCK;
    size_t   cnt = (size_t)nblocks * SECTORS_PER_BLOCK;
    int ret;
    (void)dev;
    if (nblocks == 0 || !buf) return -1;
    if (write) {
        ret = storage_write_blocks(g_xv6fs_devname, lba, buf, cnt);
    } else {
        ret = storage_read_blocks(g_xv6fs_devname, lba, buf, cnt);
    }
    if (ret != 0) {
        xv6bio_log("xv6bio: run %s error: block=%u n=%u ret=%d\n",
                   write ? "write" : "read", blockno, nblocks, ret);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

struct {
    spinlock_t     lock;
    struct xv6buf  buf[XV6FS_NBUF];
    struct xv6buf  head;
    /* 手さげ袋 (P-3)。**bget の当たり判定を O(NBUF) から O(1) にする。**
     * 2026-07-05 に NBUF を 4096 にして逆効果だったのは、線形走査の costs が
     * 全アクセスに乗ったため。ここを直したので拡大できるようになった */
    struct xv6buf *htab[XV6FS_NBUF_HASH];
} bcache;

/* 手さげ袋から外す。**入っていないこともある** (初期化直後) */
static void bhash_remove(struct xv6buf *b) {
    if (!b->hprevp) return;
    *b->hprevp = b->hnext;
    if (b->hnext) b->hnext->hprevp = b->hprevp;
    b->hnext  = 0;
    b->hprevp = 0;
}

static void bhash_insert(struct xv6buf *b) {
    struct xv6buf **head = &bcache.htab[XV6FS_BHASH(b->dev, b->blockno)];
    b->hnext = *head;
    if (*head) (*head)->hprevp = &b->hnext;
    *head = b;
    b->hprevp = head;
}

/* 在れば返す。**bcache.lock を持って呼ぶこと** */
static struct xv6buf *bhash_find(uint32_t dev, uint32_t blockno) {
    struct xv6buf *b;
    for (b = bcache.htab[XV6FS_BHASH(dev, blockno)]; b; b = b->hnext) {
        if (b->dev == dev && b->blockno == blockno) return b;
    }
    return 0;
}

/* **区間にキャッシュ済みのブロックが 1 つでもあれば 1。**
 *
 * 迂回して読むと、キャッシュ側に新しい中身がある場合に古いものを
 * 掴む。**1 つでも当たったら区間ごと従来経路に退く** —— 分割して
 * 部分的に束ねる手もあるが、判定が増えるわりに得が小さい。 */
int xv6bio_range_cached(uint32_t dev, uint32_t blockno, uint32_t nblocks) {
    uint32_t i;
    int hit = 0;
    spin_lock(&bcache.lock);
    /* **区間の各ブロックを手さげ袋で引く (P-3)。**全バッファを舐めていた
     * ころは NBUF に比例したが、区間は高々 112 ブロックなので、
     * NBUF を拡大してもこちらは増えない */
    for (i = 0; i < nblocks; i++) {
        struct xv6buf *b = bhash_find(dev, blockno + i);
        if (b && b->valid) { hit = 1; break; }
    }
    spin_unlock(&bcache.lock);
    return hit;
}

void xv6bio_init(void) {
    struct xv6buf *b;
    uint32_t i;

    spinlock_init(&bcache.lock);

    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;

    for (i = 0; i < XV6FS_NBUF_HASH; i++) bcache.htab[i] = 0;

    for (b = bcache.buf; b < bcache.buf + XV6FS_NBUF; b++) {
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        xv6_sleeplock_init(&b->lock);
        b->valid  = 0;
        b->refcnt = 0;
        b->dev    = 0xffffffffU;   /* 袋に入れない印 */
        b->blockno = 0xffffffffU;
        b->hnext  = 0;
        b->hprevp = 0;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

/* バッファを確保して返す（未キャッシュなら LRU から再利用）。
 * 戻り値はバッファロック済み。 */
static struct xv6buf *bget(uint32_t dev, uint32_t blockno) {
    struct xv6buf *b;

    spin_lock(&bcache.lock);

    /* **キャッシュ済みか確認 (P-3: 手さげ袋で O(1))。**
     * ここは全アクセスが通るので、NBUF に比例させてはいけない */
    b = bhash_find(dev, blockno);
    if (b) {
        b->refcnt++;
        spin_unlock(&bcache.lock);
        xv6_sleep_lock(&b->lock);
        return b;
    }

    /* LRU の末尾から未使用バッファを探して再利用。
     * **こちらは外したときだけ通る。**入れ替える相手は袋を移す */
    for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (b->refcnt == 0) {
            bhash_remove(b);
            b->dev     = dev;
            b->blockno = blockno;
            b->valid   = 0;
            b->refcnt  = 1;
            bhash_insert(b);
            spin_unlock(&bcache.lock);
            xv6_sleep_lock(&b->lock);
            return b;
        }
    }

    KASSERT(0 && "xv6bio bget no free buffers");
    return (struct xv6buf *)0; /* unreachable */
}

/* ブロックを読み込んでバッファを返す。呼び出し元は xv6brelse で解放すること。 */
struct xv6buf *xv6bread(uint32_t dev, uint32_t blockno) {
    struct xv6buf *b = bget(dev, blockno);
    if (!b->valid) {
        xv6fs_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

/* バッファの内容をディスクに書き出す。バッファはロック済みであること。 */
void xv6bwrite(struct xv6buf *b) {
    xv6fs_disk_rw(b, 1);
}

/* バッファのロックを解放し、LRU リストの先頭（最近使用）に移動する。 */
void xv6brelse(struct xv6buf *b) {
    xv6_sleep_unlock(&b->lock);

    spin_lock(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {
        /* LRU リストの先頭へ移動 */
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
    spin_unlock(&bcache.lock);
}

/* ログ機構がバッファをピン留めする（解放されないようにする）。 */
void xv6bpin(struct xv6buf *b) {
    spin_lock(&bcache.lock);
    b->refcnt++;
    spin_unlock(&bcache.lock);
}

/* ピン留めを解除する。 */
void xv6bunpin(struct xv6buf *b) {
    spin_lock(&bcache.lock);
    b->refcnt--;
    spin_unlock(&bcache.lock);
}
