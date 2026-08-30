/*
 * Portions of this file are derived from xv6-riscv (MIT License):
 *   Copyright (c) 2006-2019 Frans Kaashoek, Robert Morris, Russ Cox,
 *                           Massachusetts Institute of Technology
 * The full xv6 MIT license text is reproduced in THIRD_PARTY_NOTICES.md.
 */
/*
 * xv6fs.c — ファイルシステムコア
 * xv6-riscv/kernel/fs.c を Orthox-64 向けに移植。
 *
 * 変更点:
 *   sleeplock → xv6_sleeplock (wait_queue ベース。ip->lock は I/O を跨いで
 *               保持されるためスピンではなく眠って待つ)
 *   user_dst / user_src フラグ削除 (readi/writei は直接カーネルバッファ)
 *   either_copyout / either_copyin → memcpy / memmove
 *   myproc()->cwd 依存除去 (namex は絶対パスのみ対応)
 *   bmap に double indirect 追加 (addrs[NDIRECT+1])
 *   itrunc に double indirect 解放追加
 *   ROOTDEV → g_xv6fs_dev グローバル変数
 */

#include "xv6fs.h"
#include "fs.h"
#include "arch_time.h"   /* arch_time_now_ms。3 アーキテクチャに振り分ける */
#include "string.h"
#include "kassert.h"
#include <stdarg.h>

extern int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap);
extern int64_t sys_write_serial(const char *buf, size_t count);

static void xv6fs_print(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sys_write_serial(buf, (size_t)n);
}

#define min(a, b) ((uint32_t)(a) < (uint32_t)(b) ? (uint32_t)(a) : (uint32_t)(b))

/* ------------------------------------------------------------------ */
/* sleeplock 実装 (include/xv6fs.h 参照)                               */
/* ------------------------------------------------------------------ */

static int xv6_sleeplock_free(void *arg) {
    struct xv6_sleeplock *s = (struct xv6_sleeplock *)arg;
    return !s->locked;
}

void xv6_sleeplock_init(struct xv6_sleeplock *s) {
    wait_queue_init(&s->wq);
    s->locked = 0;
    s->holder_pid = 0;
}

void xv6_sleep_lock(struct xv6_sleeplock *s) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&s->wq.lock);
        if (!s->locked) {
            struct task *t = get_current_task();
            s->locked = 1;
            s->holder_pid = t ? t->pid : 0;
            spin_unlock_irqrestore(&s->wq.lock, flags);
            return;
        }
        spin_unlock_irqrestore(&s->wq.lock, flags);
        wait_event(&s->wq, xv6_sleeplock_free, s);
    }
}

void xv6_sleep_unlock(struct xv6_sleeplock *s) {
    uint64_t flags = spin_lock_irqsave(&s->wq.lock);
    s->locked = 0;
    s->holder_pid = 0;
    spin_unlock_irqrestore(&s->wq.lock, flags);
    wake_up_one(&s->wq);
}

static uint32_t xv6fs_type_mode(const struct xv6fs_inode *ip) {
    uint32_t type;
    uint32_t perm;
    if (ip->type == XV6FS_T_DEVICE) {
        /* T_DEVICE keeps real major/minor in the inode, so the
         * MODE_MAGIC permission scheme does not apply here. */
        return KSTAT_MODE_CHR | 0666U;
    }
    if (ip->type == XV6FS_T_FIFO) {
        type = KSTAT_MODE_FIFO;
        perm = 0644U;
    } else {
        type = (ip->type == XV6FS_T_DIR) ? KSTAT_MODE_DIR : KSTAT_MODE_FILE;
        perm = (ip->type == XV6FS_T_DIR) ? 0555U : 0444U;
    }
    if (ip->major == XV6FS_MODE_MAGIC) {
        perm = (uint32_t)ip->minor & 07777U;
    }
    return type | perm;
}

static void xv6fs_set_mode(struct xv6fs_inode *ip, uint32_t mode) {
    ip->major = XV6FS_MODE_MAGIC;
    ip->minor = (int16_t)(mode & 07777U);
}

/* ------------------------------------------------------------------ */
/* グローバル状態                                                      */
/* ------------------------------------------------------------------ */

static struct xv6fs_superblock g_sb;
static uint32_t g_xv6fs_dev;   /* マウント済みデバイス番号 */

/* ------------------------------------------------------------------ */
/* mtime の時計                                                        */
/*                                                                     */
/* **この機械に RTC は無い。**取れるのは起動からの経過ミリ秒だけで、        */
/* それをそのまま mtime にすると再起動のたびに 0 へ巻き戻る。            */
/* 巻き戻ると make は「ソースより出力が新しい」と誤って判断し、           */
/* 直したソースを作り直さなくなる —— 2026-08-28 に踏んだ穴そのもの。      */
/*                                                                     */
/* そこでスーパーブロックに土台 (mtime_base) を持たせ、mount のたびに     */
/* **前もって進めて書き戻す**。書き戻しをファイル書き込みより先に済ませる  */
/* ので、途中で電源が落ちても時刻が後戻りすることはない。                 */
/*                                                                     */
/* **これは壁時計ではない。**単調に増える通し番号を秒の形で持っているだけ */
/* で、実際の日時とは対応しない。make が要るのは前後関係だけなので足りる。*/
/* ------------------------------------------------------------------ */
/* mount のたびに土台をこれだけ進める。1 回の起動がこれを超えて続くと
 * 次の起動と重なりうるが、重なっても「同じ秒」になるだけで逆転はしない */
#define XV6FS_MOUNT_ADVANCE_SEC  86400U

static uint32_t g_mtime_base;

uint32_t xv6fs_now_sec(void) {
    return g_mtime_base + (uint32_t)(arch_time_now_ms() / 1000ULL);
}

struct {
    spinlock_t        lock;
    struct xv6fs_inode inode[XV6FS_NINODE];
} itable;

static void xv6fs_itrunc_to(struct xv6fs_inode *ip, uint32_t length);
void xv6fs_itrunc(struct xv6fs_inode *ip);

/* ------------------------------------------------------------------ */
/* superblock 読み込み                                                 */
/* ------------------------------------------------------------------ */

static void readsb(uint32_t dev, struct xv6fs_superblock *sb) {
    struct xv6buf *bp = xv6bread(dev, 1);
    memcpy(sb, bp->data, sizeof(*sb));
    xv6brelse(bp);
}

/* **ログを通さずに直に書く。**スーパーブロックはログ自身の位置を書いて
 * いるので、ログの中に入れると復旧の順序が循環する。書くのは mount 時の
 * mtime_base だけで、1 ブロックの上書きで済む */
static void writesb(uint32_t dev, const struct xv6fs_superblock *sb) {
    struct xv6buf *bp = xv6bread(dev, 1);
    memcpy(bp->data, sb, sizeof(*sb));
    xv6bwrite(bp);
    xv6brelse(bp);
}

/* ------------------------------------------------------------------ */
/* マウント                                                            */
/* ------------------------------------------------------------------ */

int xv6fs_mount_storage(const char *devname) {
    size_t i;
    for (i = 0; i < sizeof(g_xv6fs_devname) - 1 && devname[i]; i++)
        g_xv6fs_devname[i] = devname[i];
    g_xv6fs_devname[i] = '\0';

    g_xv6fs_dev = 1;   /* 固定: デバイス番号 1 */

    xv6bio_init();

    readsb(g_xv6fs_dev, &g_sb);
    if (g_sb.magic != XV6FS_FSMAGIC) {
        if (g_sb.magic == XV6FS_FSMAGIC_V1) {
            /* **黙って読むと inode の位置がずれて化ける。**名指しで止める */
            xv6fs_print("xv6fs: %s は mtime 以前の旧形式 (magic 0x%x)。"
                        "イメージを作り直すこと\n", devname, g_sb.magic);
        } else {
            xv6fs_print("xv6fs: bad magic 0x%x on %s\n", g_sb.magic, devname);
        }
        g_xv6fs_devname[0] = '\0';   /* マウント失敗: is_mounted() が false を返すようにクリア */
        return -1;
    }
    xv6fs_print("xv6fs: mounted %s  size=%u inodes=%u log@%u\n",
                devname, g_sb.size, g_sb.ninodes, g_sb.logstart);

    xv6log_init(g_xv6fs_dev, &g_sb);
    xv6fs_init(g_xv6fs_dev);

    /* **ファイルを 1 つも書く前に土台を進めて書き戻す。**
     * 順序が肝で、先に書き戻しておけば、この起動中に電源が落ちても
     * 次の起動の時刻はここより後ろから始まる */
    g_mtime_base = g_sb.mtime_base + XV6FS_MOUNT_ADVANCE_SEC;
    g_sb.mtime_base = g_mtime_base;
    writesb(g_xv6fs_dev, &g_sb);
    xv6fs_print("xv6fs: mtime 土台 %u 秒から\n", g_mtime_base);
    return 0;
}

/* ------------------------------------------------------------------ */
/* inode テーブル初期化                                               */
/* ------------------------------------------------------------------ */

/* **rename どうしを直列にする錠。**rename は inode を 3〜4 個まとめて
 * 掴む唯一の操作で、しかも「親を掴んでから子」の順序が守れない場合がある
 * (ディレクトリを動かすとき、相手の親が自分の子でありうる)。
 * rename は滅多に走らないので、まとめて 1 本ずつ通すのがいちばん安い */
static struct xv6_sleeplock g_rename_lock;

void xv6fs_init(uint32_t dev) {
    (void)dev;
    spinlock_init(&itable.lock);
    xv6_sleeplock_init(&g_rename_lock);
    for (int i = 0; i < XV6FS_NINODE; i++) {
        xv6_sleeplock_init(&itable.inode[i].lock);
        itable.inode[i].ref   = 0;
        itable.inode[i].valid = 0;
    }
}

/* ------------------------------------------------------------------ */
/* ブロック割り当て / 解放                                            */
/* ------------------------------------------------------------------ */

static void xv6bzero(uint32_t dev, uint32_t bno) {
    struct xv6buf *bp = xv6bread(dev, bno);
    memset(bp->data, 0, XV6FS_BSIZE);
    xv6log_write(bp);
    xv6brelse(bp);
}

static uint32_t balloc(uint32_t dev) {
    for (uint32_t b = 0; b < g_sb.size; b += XV6FS_BPB) {
        struct xv6buf *bp = xv6bread(dev, XV6FS_BBLOCK(b, g_sb));
        for (uint32_t bi = 0; bi < XV6FS_BPB && b + bi < g_sb.size; bi++) {
            int m = 1 << (bi % 8);
            if ((bp->data[bi / 8] & m) == 0) {
                bp->data[bi / 8] |= (uint8_t)m;
                xv6log_write(bp);
                xv6brelse(bp);
                xv6bzero(dev, b + bi);
                return b + bi;
            }
        }
        xv6brelse(bp);
    }
    xv6fs_print("xv6fs balloc: out of blocks\n");
    return 0;
}

static void bfree(uint32_t dev, uint32_t b) {
    struct xv6buf *bp = xv6bread(dev, XV6FS_BBLOCK(b, g_sb));
    uint32_t bi = b % XV6FS_BPB;
    int m = 1 << (bi % 8);
    KASSERT((bp->data[bi / 8] & m) != 0);
    bp->data[bi / 8] &= (uint8_t)~m;
    xv6log_write(bp);
    xv6brelse(bp);
}

/* ------------------------------------------------------------------ */
/* inode 割り当て                                                     */
/* ------------------------------------------------------------------ */

struct xv6fs_inode *xv6fs_ialloc(uint32_t dev, int16_t type) {
    for (uint32_t inum = 1; inum < g_sb.ninodes; inum++) {
        struct xv6buf *bp = xv6bread(dev, XV6FS_IBLOCK(inum, g_sb));
        struct xv6fs_dinode *dip =
            (struct xv6fs_dinode *)bp->data + inum % XV6FS_IPB;
        if (dip->type == 0) {
            memset(dip, 0, sizeof(*dip));
            dip->type  = type;
            dip->mtime = xv6fs_now_sec();
            xv6log_write(bp);
            xv6brelse(bp);
            return xv6fs_iget(dev, inum);
        }
        xv6brelse(bp);
    }
    xv6fs_print("xv6fs ialloc: no inodes\n");
    return (struct xv6fs_inode *)0;
}

/* ------------------------------------------------------------------ */
/* inode キャッシュ管理                                               */
/* ------------------------------------------------------------------ */

struct xv6fs_inode *xv6fs_iget(uint32_t dev, uint32_t inum) {
    struct xv6fs_inode *ip, *empty;

    spin_lock(&itable.lock);

    empty = (struct xv6fs_inode *)0;
    for (ip = &itable.inode[0]; ip < &itable.inode[XV6FS_NINODE]; ip++) {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;
            spin_unlock(&itable.lock);
            return ip;
        }
        if (!empty && ip->ref == 0)
            empty = ip;
    }

    KASSERT(empty != 0);

    ip = empty;
    ip->dev   = dev;
    ip->inum  = inum;
    ip->ref   = 1;
    ip->valid = 0;
    spin_unlock(&itable.lock);
    return ip;
}

void xv6fs_ilock(struct xv6fs_inode *ip) {
    KASSERT(ip != 0);
    KASSERT(ip->ref >= 1);

    xv6_sleep_lock(&ip->lock);

    if (!ip->valid) {
        struct xv6buf *bp = xv6bread(ip->dev, XV6FS_IBLOCK(ip->inum, g_sb));
        struct xv6fs_dinode *dip =
            (struct xv6fs_dinode *)bp->data + ip->inum % XV6FS_IPB;
        ip->type  = dip->type;
        ip->major = dip->major;
        ip->minor = dip->minor;
        ip->nlink = dip->nlink;
        ip->size  = dip->size;
        ip->mtime = dip->mtime;
        memcpy(ip->addrs, dip->addrs, sizeof(ip->addrs));
        xv6brelse(bp);
        ip->valid = 1;
        KASSERT(ip->type != 0);
    }
}

void xv6fs_iunlock(struct xv6fs_inode *ip) {
    KASSERT(ip != 0);
    KASSERT(ip->ref >= 1);
    xv6_sleep_unlock(&ip->lock);
}

void xv6fs_iupdate(struct xv6fs_inode *ip) {
    struct xv6buf *bp = xv6bread(ip->dev, XV6FS_IBLOCK(ip->inum, g_sb));
    struct xv6fs_dinode *dip =
        (struct xv6fs_dinode *)bp->data + ip->inum % XV6FS_IPB;
    dip->type  = ip->type;
    dip->major = ip->major;
    dip->minor = ip->minor;
    dip->nlink = ip->nlink;
    dip->size  = ip->size;
    dip->mtime = ip->mtime;
    memcpy(dip->addrs, ip->addrs, sizeof(ip->addrs));
    xv6log_write(bp);
    xv6brelse(bp);
}

void xv6fs_iput(struct xv6fs_inode *ip) {
    spin_lock(&itable.lock);

    if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
        /* 参照が自分だけ + リンク数 0 → truncate して解放 */
        xv6_sleep_lock(&ip->lock);
        spin_unlock(&itable.lock);

        xv6fs_itrunc(ip);
        ip->type = 0;
        xv6fs_iupdate(ip);
        ip->valid = 0;

        xv6_sleep_unlock(&ip->lock);
        spin_lock(&itable.lock);
    }

    ip->ref--;
    spin_unlock(&itable.lock);
}

void xv6fs_iunlockput(struct xv6fs_inode *ip) {
    xv6fs_iunlock(ip);
    xv6fs_iput(ip);
}

/* ------------------------------------------------------------------ */
/* bmap — 論理ブロック番号 → 物理ブロック番号 (triple indirect 対応) */
/* ------------------------------------------------------------------ */

static uint32_t bmap_lookup(struct xv6fs_inode *ip, uint32_t bn, int alloc) {
    uint32_t addr;
    struct xv6buf *bp;
    uint32_t *a;

    /* 直接ブロック */
    if (bn < XV6FS_NDIRECT) {
        if ((addr = ip->addrs[bn]) == 0) {
            if (!alloc) return 0;
            addr = balloc(ip->dev);
            if (addr == 0) return 0;
            ip->addrs[bn] = addr;
        }
        return addr;
    }
    bn -= XV6FS_NDIRECT;

    /* 1段間接ブロック */
    if (bn < XV6FS_NINDIRECT) {
        if ((addr = ip->addrs[XV6FS_NDIRECT]) == 0) {
            if (!alloc) return 0;
            addr = balloc(ip->dev);
            if (addr == 0) return 0;
            ip->addrs[XV6FS_NDIRECT] = addr;
        }
        bp = xv6bread(ip->dev, addr);
        a  = (uint32_t *)bp->data;
        if ((addr = a[bn]) == 0) {
            if (!alloc) {
                xv6brelse(bp);
                return 0;
            }
            addr = balloc(ip->dev);
            if (addr) {
                a[bn] = addr;
                xv6log_write(bp);
            }
        }
        xv6brelse(bp);
        return addr;
    }
    bn -= XV6FS_NINDIRECT;

    /* 2段間接ブロック */
    if (bn < XV6FS_NDINDIRECT) {
        if ((addr = ip->addrs[XV6FS_NDIRECT + 1]) == 0) {
            if (!alloc) return 0;
            addr = balloc(ip->dev);
            if (addr == 0) return 0;
            ip->addrs[XV6FS_NDIRECT + 1] = addr;
        }
        /* L1 間接テーブル */
        bp = xv6bread(ip->dev, addr);
        a  = (uint32_t *)bp->data;
        uint32_t i2 = bn / XV6FS_NINDIRECT;
        if ((addr = a[i2]) == 0) {
            if (!alloc) {
                xv6brelse(bp);
                return 0;
            }
            addr = balloc(ip->dev);
            if (addr) {
                a[i2] = addr;
                xv6log_write(bp);
            }
        }
        xv6brelse(bp);
        if (addr == 0) return 0;

        /* L2 間接テーブル */
        bp = xv6bread(ip->dev, addr);
        a  = (uint32_t *)bp->data;
        uint32_t i1 = bn % XV6FS_NINDIRECT;
        if ((addr = a[i1]) == 0) {
            if (!alloc) {
                xv6brelse(bp);
                return 0;
            }
            addr = balloc(ip->dev);
            if (addr) {
                a[i1] = addr;
                xv6log_write(bp);
            }
        }
        xv6brelse(bp);
        return addr;
    }
    bn -= XV6FS_NDINDIRECT;

    /* 3段間接ブロック */
    if (bn < XV6FS_NTINDIRECT) {
        if ((addr = ip->addrs[XV6FS_NDIRECT + 2]) == 0) {
            if (!alloc) return 0;
            addr = balloc(ip->dev);
            if (addr == 0) return 0;
            ip->addrs[XV6FS_NDIRECT + 2] = addr;
        }
        /* L1 */
        bp = xv6bread(ip->dev, addr);
        a  = (uint32_t *)bp->data;
        uint32_t i3 = bn / XV6FS_NDINDIRECT;
        if ((addr = a[i3]) == 0) {
            if (!alloc) {
                xv6brelse(bp);
                return 0;
            }
            addr = balloc(ip->dev);
            if (addr) { a[i3] = addr; xv6log_write(bp); }
        }
        xv6brelse(bp);
        if (addr == 0) return 0;

        /* L2 */
        bp = xv6bread(ip->dev, addr);
        a  = (uint32_t *)bp->data;
        uint32_t i2t = (bn % XV6FS_NDINDIRECT) / XV6FS_NINDIRECT;
        if ((addr = a[i2t]) == 0) {
            if (!alloc) {
                xv6brelse(bp);
                return 0;
            }
            addr = balloc(ip->dev);
            if (addr) { a[i2t] = addr; xv6log_write(bp); }
        }
        xv6brelse(bp);
        if (addr == 0) return 0;

        /* L3 */
        bp = xv6bread(ip->dev, addr);
        a  = (uint32_t *)bp->data;
        uint32_t i1t = bn % XV6FS_NINDIRECT;
        if ((addr = a[i1t]) == 0) {
            if (!alloc) {
                xv6brelse(bp);
                return 0;
            }
            addr = balloc(ip->dev);
            if (addr) { a[i1t] = addr; xv6log_write(bp); }
        }
        xv6brelse(bp);
        return addr;
    }

    KASSERT(0 && "xv6fs bmap out of range");
    return 0;
}

static uint32_t bmap(struct xv6fs_inode *ip, uint32_t bn) {
    return bmap_lookup(ip, bn, 1);
}

static int block_table_empty(uint32_t *a) {
    for (int i = 0; i < (int)XV6FS_NINDIRECT; i++) {
        if (a[i] != 0) return 0;
    }
    return 1;
}

static void xv6fs_itrunc_to(struct xv6fs_inode *ip, uint32_t length) {
    uint32_t keep = (length + XV6FS_BSIZE - 1) / XV6FS_BSIZE;

    /* 直接ブロック */
    for (int i = 0; i < XV6FS_NDIRECT; i++) {
        if ((uint32_t)i >= keep && ip->addrs[i]) {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    /* 1段間接ブロック */
    if (ip->addrs[XV6FS_NDIRECT]) {
        struct xv6buf *bp = xv6bread(ip->dev, ip->addrs[XV6FS_NDIRECT]);
        uint32_t *a = (uint32_t *)bp->data;
        int dirty = 0;
        for (int j = 0; j < (int)XV6FS_NINDIRECT; j++) {
            uint32_t fbn = XV6FS_NDIRECT + (uint32_t)j;
            if (fbn >= keep && a[j]) {
                bfree(ip->dev, a[j]);
                a[j] = 0;
                dirty = 1;
            }
        }
        if (block_table_empty(a)) {
            xv6brelse(bp);
            bfree(ip->dev, ip->addrs[XV6FS_NDIRECT]);
            ip->addrs[XV6FS_NDIRECT] = 0;
        } else {
            if (dirty) xv6log_write(bp);
            xv6brelse(bp);
        }
    }

    /* 2段間接ブロック */
    if (ip->addrs[XV6FS_NDIRECT + 1]) {
        struct xv6buf *bp = xv6bread(ip->dev, ip->addrs[XV6FS_NDIRECT + 1]);
        uint32_t *a = (uint32_t *)bp->data;
        int dirty_l1 = 0;
        for (int i2 = 0; i2 < (int)XV6FS_NINDIRECT; i2++) {
            if (a[i2] == 0) continue;
            struct xv6buf *bp2 = xv6bread(ip->dev, a[i2]);
            uint32_t *a2 = (uint32_t *)bp2->data;
            int dirty_l2 = 0;
            for (int i1 = 0; i1 < (int)XV6FS_NINDIRECT; i1++) {
                uint32_t rel = (uint32_t)i2 * XV6FS_NINDIRECT + (uint32_t)i1;
                uint32_t fbn = XV6FS_NDIRECT + XV6FS_NINDIRECT + rel;
                if (fbn >= keep && a2[i1]) {
                    bfree(ip->dev, a2[i1]);
                    a2[i1] = 0;
                    dirty_l2 = 1;
                }
            }
            if (block_table_empty(a2)) {
                xv6brelse(bp2);
                bfree(ip->dev, a[i2]);
                a[i2] = 0;
                dirty_l1 = 1;
            } else {
                if (dirty_l2) xv6log_write(bp2);
                xv6brelse(bp2);
            }
        }
        if (block_table_empty(a)) {
            xv6brelse(bp);
            bfree(ip->dev, ip->addrs[XV6FS_NDIRECT + 1]);
            ip->addrs[XV6FS_NDIRECT + 1] = 0;
        } else {
            if (dirty_l1) xv6log_write(bp);
            xv6brelse(bp);
        }
    }

    /* 3段間接ブロック */
    if (ip->addrs[XV6FS_NDIRECT + 2]) {
        struct xv6buf *bp = xv6bread(ip->dev, ip->addrs[XV6FS_NDIRECT + 2]);
        uint32_t *a = (uint32_t *)bp->data;
        int dirty_l1 = 0;
        for (int i3 = 0; i3 < (int)XV6FS_NINDIRECT; i3++) {
            if (a[i3] == 0) continue;
            struct xv6buf *bpL2 = xv6bread(ip->dev, a[i3]);
            uint32_t *aL2 = (uint32_t *)bpL2->data;
            int dirty_l2 = 0;
            for (int i2 = 0; i2 < (int)XV6FS_NINDIRECT; i2++) {
                if (aL2[i2] == 0) continue;
                struct xv6buf *bpL3 = xv6bread(ip->dev, aL2[i2]);
                uint32_t *aL3 = (uint32_t *)bpL3->data;
                int dirty_l3 = 0;
                for (int i1 = 0; i1 < (int)XV6FS_NINDIRECT; i1++) {
                    uint32_t rel = (uint32_t)i3 * XV6FS_NDINDIRECT +
                                   (uint32_t)i2 * XV6FS_NINDIRECT +
                                   (uint32_t)i1;
                    uint32_t fbn = XV6FS_NDIRECT + XV6FS_NINDIRECT +
                                   XV6FS_NDINDIRECT + rel;
                    if (fbn >= keep && aL3[i1]) {
                        bfree(ip->dev, aL3[i1]);
                        aL3[i1] = 0;
                        dirty_l3 = 1;
                    }
                }
                if (block_table_empty(aL3)) {
                    xv6brelse(bpL3);
                    bfree(ip->dev, aL2[i2]);
                    aL2[i2] = 0;
                    dirty_l2 = 1;
                } else {
                    if (dirty_l3) xv6log_write(bpL3);
                    xv6brelse(bpL3);
                }
            }
            if (block_table_empty(aL2)) {
                xv6brelse(bpL2);
                bfree(ip->dev, a[i3]);
                a[i3] = 0;
                dirty_l1 = 1;
            } else {
                if (dirty_l2) xv6log_write(bpL2);
                xv6brelse(bpL2);
            }
        }
        if (block_table_empty(a)) {
            xv6brelse(bp);
            bfree(ip->dev, ip->addrs[XV6FS_NDIRECT + 2]);
            ip->addrs[XV6FS_NDIRECT + 2] = 0;
        } else {
            if (dirty_l1) xv6log_write(bp);
            xv6brelse(bp);
        }
    }

    ip->size  = length;
    ip->mtime = xv6fs_now_sec();
    xv6fs_iupdate(ip);
}

/* ------------------------------------------------------------------ */
/* itrunc — ファイルの全ブロックを解放                               */
/* ------------------------------------------------------------------ */

void xv6fs_itrunc(struct xv6fs_inode *ip) {
    xv6fs_itrunc_to(ip, 0);
}

/* ------------------------------------------------------------------ */
/* stat                                                                */
/* ------------------------------------------------------------------ */

int xv6fs_stat(struct xv6fs_inode *ip, struct xv6fs_stat *st) {
    st->dev   = ip->dev;
    st->ino   = ip->inum;
    st->type  = ip->type;
    st->nlink = ip->nlink;
    st->size  = ip->size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* readi / writei                                                     */
/* ------------------------------------------------------------------ */

int xv6fs_readi(struct xv6fs_inode *ip, void *dst, uint32_t off, uint32_t n) {
    uint32_t tot, m;
    struct xv6buf *bp;

    if (off > ip->size || off + n < off)
        return 0;
    if (off + n > ip->size)
        n = ip->size - off;

    for (tot = 0; tot < n; tot += m, off += m, dst = (uint8_t *)dst + m) {
        uint32_t addr = bmap_lookup(ip, off / XV6FS_BSIZE, 0);
        m  = min(n - tot, XV6FS_BSIZE - off % XV6FS_BSIZE);
        if (addr == 0) {
            memset(dst, 0, m);
            continue;
        }

        /* **連続している区間はまとめて 1 コマンドで運ぶ (P-5)。**
         *
         * 条件は 3 つ。ブロックの境目に揃っていること、丸ごと 1 ブロック
         * 以上読むこと、そして先の物理ブロックが連番であること。
         * bmap は逐次確保するので、素直に書かれたファイルはたいてい連番。
         *
         * **キャッシュを迂回するので、区間に 1 つでも載っていたら退く。**
         * 載っている側が新しい可能性があり、古いものを掴む。 */
        if ((off % XV6FS_BSIZE) == 0 && (n - tot) >= XV6FS_BSIZE) {
            uint32_t run = 1;
            uint32_t maxrun = (n - tot) / XV6FS_BSIZE;
            while (run < maxrun) {
                uint32_t a2 = bmap_lookup(ip, off / XV6FS_BSIZE + run, 0);
                if (a2 != addr + run) break;
                run++;
            }
            if (run > 1 && !xv6bio_range_cached(ip->dev, addr, run) &&
                xv6bio_rw_run(ip->dev, addr, run, dst, 0) == 0) {
                m = run * XV6FS_BSIZE;
                continue;
            }
        }

        bp = xv6bread(ip->dev, addr);
        memcpy(dst, bp->data + (off % XV6FS_BSIZE), m);
        xv6brelse(bp);
    }
    return (int)tot;
}

int xv6fs_writei(struct xv6fs_inode *ip, const void *src,
                 uint32_t off, uint32_t n) {
    uint32_t tot, m;
    struct xv6buf *bp;
    uint64_t end = (uint64_t)off + (uint64_t)n;
    uint64_t max_bytes = (uint64_t)XV6FS_MAXFILE * (uint64_t)XV6FS_BSIZE;

    if (off + n < off)
        return -1;
    if (end > max_bytes)
        return -1;

    for (tot = 0; tot < n; tot += m, off += m, src = (const uint8_t *)src + m) {
        uint32_t addr = bmap(ip, off / XV6FS_BSIZE);
        if (addr == 0) break;
        bp = xv6bread(ip->dev, addr);
        m  = min(n - tot, XV6FS_BSIZE - off % XV6FS_BSIZE);
        memcpy(bp->data + (off % XV6FS_BSIZE), src, m);
        xv6log_write(bp);
        xv6brelse(bp);
    }

    if (off > ip->size)
        ip->size = off;
    /* **make が見ているのはこれ。**書いた瞬間に時刻を進めないと、
     * 出力が入力より新しくならず依存解決が成り立たない */
    if (tot > 0)
        ip->mtime = xv6fs_now_sec();
    xv6fs_iupdate(ip);
    return (int)tot;
}

/* ------------------------------------------------------------------ */
/* ディレクトリ操作                                                   */
/* ------------------------------------------------------------------ */

static int namecmp(const char *s, const char *t) {
    return strncmp(s, t, XV6FS_DIRSIZ);
}

struct xv6fs_inode *xv6fs_dirlookup(struct xv6fs_inode *dp,
                                     const char *name, uint32_t *poff) {
    struct xv6fs_dirent de;

    if (dp->type != XV6FS_T_DIR) {
        return (struct xv6fs_inode *)0;
    }

    for (uint32_t off = 0; off < dp->size; off += sizeof(de)) {
        if (xv6fs_readi(dp, &de, off, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum == 0)
            continue;
        if (namecmp(name, de.name) == 0) {
            if (poff) *poff = off;
            return xv6fs_iget(dp->dev, de.inum);
        }
    }
    return (struct xv6fs_inode *)0;
}

int xv6fs_dirlink(struct xv6fs_inode *dp, const char *name, uint32_t inum) {
    struct xv6fs_dirent de;
    uint32_t off;

    /* 同名エントリが既にあれば失敗 */
    struct xv6fs_inode *ip = xv6fs_dirlookup(dp, name, 0);
    if (ip) {
        xv6fs_iput(ip);
        return -1;
    }

    /* 空きエントリを探す */
    for (off = 0; off < dp->size; off += sizeof(de)) {
        KASSERT(xv6fs_readi(dp, &de, off, sizeof(de)) == (int)sizeof(de));
        if (de.inum == 0)
            break;
    }

    memset(de.name, 0, XV6FS_DIRSIZ);
    strncpy(de.name, name, XV6FS_DIRSIZ);
    de.inum = (uint16_t)inum;
    if (xv6fs_writei(dp, &de, off, sizeof(de)) != (int)sizeof(de))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* パス解決                                                           */
/* ------------------------------------------------------------------ */

static const char *skipelem(const char *path, char *name) {
    while (*path == '/') path++;
    if (*path == '\0') return (const char *)0;

    const char *s = path;
    while (*path != '/' && *path != '\0') path++;

    int len = (int)(path - s);
    if (len >= XV6FS_DIRSIZ) {
        memcpy(name, s, XV6FS_DIRSIZ);
    } else {
        memcpy(name, s, (size_t)len);
        name[len] = '\0';
    }

    while (*path == '/') path++;
    return path;
}

/*
 * namex: 絶対パスのみ対応（myproc()->cwd 依存を除去）
 */
static struct xv6fs_inode *namex(const char *path, int nameiparent, char *name) {
    struct xv6fs_inode *ip, *next;

    if (*path == '/')
        ip = xv6fs_iget(g_xv6fs_dev, XV6FS_ROOTINO);
    else
        return (struct xv6fs_inode *)0;   /* 相対パス未対応 */

    while ((path = skipelem(path, name)) != (const char *)0) {
        xv6fs_ilock(ip);
        if (ip->type != XV6FS_T_DIR) {
            xv6fs_iunlockput(ip);
            return (struct xv6fs_inode *)0;
        }
        if (nameiparent && *path == '\0') {
            xv6fs_iunlock(ip);
            return ip;
        }
        next = xv6fs_dirlookup(ip, name, (uint32_t *)0);
        xv6fs_iunlockput(ip);
        if (!next)
            return (struct xv6fs_inode *)0;
        ip = next;
    }

    if (nameiparent) {
        xv6fs_iput(ip);
        return (struct xv6fs_inode *)0;
    }
    return ip;
}

struct xv6fs_inode *xv6fs_namei(const char *path) {
    char name[XV6FS_DIRSIZ];
    return namex(path, 0, name);
}

struct xv6fs_inode *xv6fs_nameiparent(const char *path, char *name) {
    return namex(path, 1, name);
}

/* ================================================================== */
/* VFS アダプタ — kernel/fs.c が xv6fs を使うための高レベル I/F      */
/* ================================================================== */

int xv6fs_is_mounted(void) {
    return g_xv6fs_devname[0] != '\0';
}

int xv6fs_ino_path(const char *path, uint64_t *out_ino) {
    const char *lookup = (path && path[0]) ? path : "/";
    struct xv6fs_inode *ip = xv6fs_namei(lookup);
    if (!ip) return -1;
    xv6fs_ilock(ip);
    if (out_ino) *out_ino = (uint64_t)ip->inum;
    xv6fs_iunlock(ip);
    xv6fs_iput(ip);
    return 0;
}

int xv6fs_stat_path(const char *path, uint32_t *out_mode,
                    uint64_t *out_size, int64_t *out_mtime,
                    uint32_t *out_rdev) {
    const char *lookup = (path && path[0]) ? path : "/";
    struct xv6fs_inode *ip = xv6fs_namei(lookup);
    if (!ip) return -1;
    xv6fs_ilock(ip);
    if (out_mode)  *out_mode  = xv6fs_type_mode(ip);
    if (out_size)  *out_size  = ip->size;
    if (out_mtime) *out_mtime = (int64_t)ip->mtime;
    if (out_rdev) {
        *out_rdev = (ip->type == XV6FS_T_DEVICE)
            ? ((((uint32_t)ip->major & 0xFFU) << 8) | ((uint32_t)ip->minor & 0xFFU))
            : 0;
    }
    xv6fs_iunlock(ip);
    xv6fs_iput(ip);
    return 0;
}

int xv6fs_list_dir(const char *path, struct orth_dirent *dirents,
                   size_t max_entries, size_t *out_count) {
    const char *lookup = (path && path[0]) ? path : "/";
    struct xv6fs_inode *dp = xv6fs_namei(lookup);
    if (!dp) return -1;
    xv6fs_ilock(dp);
    if (dp->type != XV6FS_T_DIR) {
        xv6fs_iunlock(dp); xv6fs_iput(dp); return -1;
    }

    size_t count = out_count ? *out_count : 0;
    struct xv6fs_dirent de;

    for (uint32_t off = 0; off < dp->size; off += sizeof(de)) {
        if (xv6fs_readi(dp, &de, off, sizeof(de)) != (int)sizeof(de)) break;
        if (de.inum == 0) continue;
        if (de.name[0] == '.' &&
            (de.name[1] == '\0' || (de.name[1] == '.' && de.name[2] == '\0')))
            continue;
        if (count >= max_entries) break;

        struct xv6fs_inode *ip = xv6fs_iget(dp->dev, de.inum);
        xv6fs_ilock(ip);
        uint32_t mode = xv6fs_type_mode(ip);
        uint32_t sz   = ip->size;
        xv6fs_iunlock(ip);
        xv6fs_iput(ip);

        dirents[count].mode = mode;
        dirents[count].size = sz;
        int ni;
        for (ni = 0; ni < (int)sizeof(dirents[count].name) - 1 &&
                     ni < XV6FS_DIRSIZ && de.name[ni]; ni++)
            dirents[count].name[ni] = de.name[ni];
        dirents[count].name[ni] = '\0';
        count++;
    }

    xv6fs_iunlock(dp);
    xv6fs_iput(dp);
    if (out_count) *out_count = count;
    return 0;
}

int xv6fs_write_file(const char *path, uint64_t offset,
                     const void *buf, size_t n) {
    struct xv6fs_inode *ip = xv6fs_namei(path);
    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;

    if (!ip) return -1;

    /* n == 0 でも xv6fs_writei を 1 回通す (サイズ更新などの副作用を保つ) */
    do {
        size_t chunk = n - done;
        int r;
        if (chunk > (size_t)XV6FS_WRITE_CHUNK_MAX) chunk = (size_t)XV6FS_WRITE_CHUNK_MAX;

        /* **申告は「最大」ではなく「今回の chunk」で出す (P-6)。**
         * 小さな書きが小さく申告するからこそ、溜めたまま入れる */
        int op_blocks = XV6LOG_OP_WRITE(chunk);
        xv6log_begin_op(op_blocks);
        xv6fs_ilock(ip);
        r = xv6fs_writei(ip, src + done, (uint32_t)(offset + done), (uint32_t)chunk);
        xv6fs_iunlock(ip);
        xv6log_end_op(op_blocks);

        if (r != (int)chunk) {
            xv6fs_iput(ip);
            return -1;
        }
        done += chunk;
    } while (done < n);

    xv6fs_iput(ip);
    return 0;
}

/* mkfifo: T_FIFO inode を作成する。既存パスなら -17 (EEXIST)。 */
int xv6fs_mknod_fifo(const char *path, int mode) {
    char name[XV6FS_DIRSIZ];
    struct xv6fs_inode *dp;
    struct xv6fs_inode *ip;

    if (!path || path[0] == '\0') return -1;

    xv6log_begin_op(XV6LOG_OP_SMALL);
    dp = xv6fs_nameiparent(path, name);
    if (!dp) {
        xv6log_end_op(XV6LOG_OP_SMALL);
        return -1;
    }

    xv6fs_ilock(dp);
    ip = xv6fs_dirlookup(dp, name, 0);
    if (ip) {
        xv6fs_iput(ip);
        xv6fs_iunlock(dp);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(dp);
        return -17; /* EEXIST */
    }

    ip = xv6fs_ialloc(dp->dev, XV6FS_T_FIFO);
    if (!ip) {
        xv6fs_iunlock(dp);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(dp);
        return -1;
    }
    xv6fs_ilock(ip);
    ip->nlink = 1;
    xv6fs_set_mode(ip, (uint32_t)mode);
    xv6fs_iupdate(ip);
    if (xv6fs_dirlink(dp, name, ip->inum) < 0) {
        ip->nlink = 0;
        xv6fs_iupdate(ip);
        xv6fs_iunlock(ip);
        xv6fs_iput(ip);
        xv6fs_iunlock(dp);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(dp);
        return -1;
    }
    xv6fs_iunlock(ip);
    xv6fs_iunlock(dp);
    xv6log_end_op(XV6LOG_OP_SMALL);
    xv6fs_iput(dp);
    xv6fs_iput(ip);
    return 0;
}

int xv6fs_create_file(const char *path, int mode,
                      struct xv6fs_inode **out_ip) {
    char name[XV6FS_DIRSIZ];
    struct xv6fs_inode *dp;
    struct xv6fs_inode *ip;

    if (out_ip) *out_ip = 0;
    if (!path || path[0] == '\0') return -1;

    xv6log_begin_op(XV6LOG_OP_SMALL);
    dp = xv6fs_nameiparent(path, name);
    if (!dp) {
        xv6log_end_op(XV6LOG_OP_SMALL);
        return -1;
    }

    xv6fs_ilock(dp);
    ip = xv6fs_dirlookup(dp, name, 0);
    if (ip) {
        xv6fs_ilock(ip);
        /* Existing T_DEVICE / T_FIFO nodes are returned as-is so that
         * O_CREAT opens of e.g. /dev/null or a named pipe do not fall
         * back to ramfs and shadow the node. */
        if (ip->type != XV6FS_T_FILE && ip->type != XV6FS_T_DEVICE &&
            ip->type != XV6FS_T_FIFO) {
            xv6fs_iunlock(ip);
            xv6fs_iput(ip);
            xv6fs_iunlock(dp);
            xv6log_end_op(XV6LOG_OP_SMALL);
            xv6fs_iput(dp);
            return -1;
        }
        xv6fs_iunlock(ip);
        xv6fs_iunlock(dp);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(dp);
        if (out_ip) *out_ip = ip;
        else xv6fs_iput(ip);
        return 0;
    }

    ip = xv6fs_ialloc(dp->dev, XV6FS_T_FILE);
    if (!ip) {
        xv6fs_iunlock(dp);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(dp);
        return -1;
    }
    xv6fs_ilock(ip);
    ip->nlink = 1;
    xv6fs_set_mode(ip, (uint32_t)mode);
    xv6fs_iupdate(ip);
    if (xv6fs_dirlink(dp, name, ip->inum) < 0) {
        ip->nlink = 0;
        xv6fs_iupdate(ip);
        xv6fs_iunlock(ip);
        xv6fs_iput(ip);
        xv6fs_iunlock(dp);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(dp);
        return -1;
    }
    xv6fs_iunlock(ip);
    xv6fs_iunlock(dp);
    xv6log_end_op(XV6LOG_OP_SMALL);
    xv6fs_iput(dp);

    if (out_ip) *out_ip = ip;
    else xv6fs_iput(ip);
    return 0;
}

int xv6fs_truncate_file(const char *path, uint64_t length) {
    struct xv6fs_inode *ip = xv6fs_namei(path);
    if (!ip) return -1;
    xv6log_begin_op(XV6LOG_OP_FULL);
    xv6fs_ilock(ip);
    if (length > (uint64_t)XV6FS_MAXFILE * XV6FS_BSIZE) {
        xv6fs_iunlock(ip);
        xv6log_end_op(XV6LOG_OP_FULL);
        xv6fs_iput(ip);
        return -1;
    }
    if (length < ip->size) {
        xv6fs_itrunc_to(ip, (uint32_t)length);
    } else {
        ip->size  = (uint32_t)length;
        ip->mtime = xv6fs_now_sec();
        xv6fs_iupdate(ip);
    }
    xv6fs_iunlock(ip);
    xv6log_end_op(XV6LOG_OP_FULL);
    xv6fs_iput(ip);
    return 0;
}

int xv6fs_unlink_path(const char *path) {
    char name[XV6FS_DIRSIZ];
    struct xv6fs_inode *dp = xv6fs_nameiparent(path, name);
    if (!dp) return -1;

    xv6log_begin_op(XV6LOG_OP_FULL);
    xv6fs_ilock(dp);
    uint32_t off = 0;
    struct xv6fs_inode *ip = xv6fs_dirlookup(dp, name, &off);
    if (!ip) {
        xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(dp); return -1;
    }
    xv6fs_ilock(ip);
    if (ip->type == XV6FS_T_DIR) {
        xv6fs_iunlock(ip); xv6fs_iput(ip);
        xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(dp); return -1;
    }
    struct xv6fs_dirent de;
    memset(&de, 0, sizeof(de));
    xv6fs_writei(dp, &de, off, sizeof(de));
    ip->nlink--;
    xv6fs_iupdate(ip);
    xv6fs_iunlock(ip); xv6fs_iput(ip);
    xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(dp);
    return 0;
}

int xv6fs_rmdir_path(const char *path) {
    char name[XV6FS_DIRSIZ];
    struct xv6fs_inode *dp = xv6fs_nameiparent(path, name);
    if (!dp) return -1;

    xv6log_begin_op(XV6LOG_OP_FULL);
    xv6fs_ilock(dp);
    uint32_t off = 0;
    struct xv6fs_inode *ip = xv6fs_dirlookup(dp, name, &off);
    if (!ip) {
        xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(dp); return -1;
    }
    xv6fs_ilock(ip);
    if (ip->type != XV6FS_T_DIR) {
        xv6fs_iunlock(ip); xv6fs_iput(ip);
        xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(dp); return -1;
    }
    struct xv6fs_dirent de;
    memset(&de, 0, sizeof(de));
    xv6fs_writei(dp, &de, off, sizeof(de));
    dp->nlink--;
    xv6fs_iupdate(dp);
    ip->nlink = 0;
    xv6fs_iupdate(ip);
    xv6fs_iunlock(ip); xv6fs_iput(ip);
    xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(dp);
    return 0;
}

/* ハードリンクを張る。xv6 本家の sys_link と同じ手順:
 * 先に nlink を増やしてから dirlink し、失敗したら巻き戻す。
 * (逆順だと dirlink 成功後にクラッシュした場合 nlink が実数より少なくなる) */
int xv6fs_link_path(const char *oldpath, const char *newpath) {
    char name[XV6FS_DIRSIZ];
    struct xv6fs_inode *ip = xv6fs_namei(oldpath);
    struct xv6fs_inode *dp;

    if (!ip) return -1;

    xv6log_begin_op(XV6LOG_OP_FULL);
    xv6fs_ilock(ip);
    /* ディレクトリのハードリンクはループを作るので禁止 */
    if (ip->type == XV6FS_T_DIR) {
        xv6fs_iunlock(ip); xv6log_end_op(XV6LOG_OP_FULL); xv6fs_iput(ip); return -1;
    }
    ip->nlink++;
    xv6fs_iupdate(ip);
    xv6fs_iunlock(ip);

    dp = xv6fs_nameiparent(newpath, name);
    if (!dp) goto bad;
    xv6fs_ilock(dp);
    if (dp->dev != ip->dev || xv6fs_dirlink(dp, name, ip->inum) < 0) {
        xv6fs_iunlock(dp); xv6fs_iput(dp);
        goto bad;
    }
    xv6fs_iunlock(dp); xv6fs_iput(dp);
    xv6log_end_op(XV6LOG_OP_FULL);
    xv6fs_iput(ip);
    return 0;

bad:
    xv6fs_ilock(ip);
    ip->nlink--;
    xv6fs_iupdate(ip);
    xv6fs_iunlock(ip);
    xv6log_end_op(XV6LOG_OP_FULL);
    xv6fs_iput(ip);
    return -1;
}

/* ------------------------------------------------------------------ */
/* rename                                                             */
/* ------------------------------------------------------------------ */

/* "." と ".." のほかに何も入っていないか */
static int dir_is_empty(struct xv6fs_inode *dp) {
    struct xv6fs_dirent de;

    for (uint32_t off = 0; off < dp->size; off += sizeof(de)) {
        if (xv6fs_readi(dp, &de, off, sizeof(de)) != (int)sizeof(de))
            return 0;
        if (de.inum == 0) continue;
        if (namecmp(de.name, ".") == 0 || namecmp(de.name, "..") == 0) continue;
        return 0;
    }
    return 1;
}

/* ディレクトリを別の親へ移したとき、中の ".." を張り替える */
static int dir_reparent(struct xv6fs_inode *dp, uint32_t parent_inum) {
    struct xv6fs_dirent de;

    for (uint32_t off = 0; off < dp->size; off += sizeof(de)) {
        if (xv6fs_readi(dp, &de, off, sizeof(de)) != (int)sizeof(de))
            break;
        if (de.inum == 0 || namecmp(de.name, "..") != 0) continue;
        de.inum = (uint16_t)parent_inum;
        return xv6fs_writei(dp, &de, off, sizeof(de)) == (int)sizeof(de) ? 0 : -1;
    }
    return -1;
}

/* rename(2)。**xv6fs には rename が無く、上の層は EXDEV を返して
 * mv の copy+unlink に退いていた。**これがその実装。
 *
 * 付け替えを 1 つのログトランザクションで行うので、途中で電源が落ちても
 * 「古い名前のまま」か「新しい名前になった」かのどちらかにしかならない。
 *
 * 満たすもの:
 *   - 相手が既にあれば黙って置き換える (ディレクトリなら空のときだけ)
 *   - 同じ実体を指す名前どうしなら何もせずに成功 (POSIX の規定)
 *   - ディレクトリを別の親へ移したら中の ".." を張り替える
 *   - 種別が食い違えば ENOTDIR / EISDIR で断る
 *
 * 呼び出し側 (fs_rename) が済ませておくこと:
 *   - 絶対パスへの正規化
 *   - **自分の子孫への移動を弾くこと。**このファイルシステムはディレクトリの
 *     ハードリンクを許さないので実体とパスが 1 対 1 になり、文字列の前方一致
 *     で判定できる
 *
 * 戻り値は 0 か負の errno。
 */
int xv6fs_rename_path(const char *oldpath, const char *newpath) {
    char old_name[XV6FS_DIRSIZ];
    char new_name[XV6FS_DIRSIZ];
    struct xv6fs_inode *dp_old, *dp_new;
    struct xv6fs_inode *ip = (struct xv6fs_inode *)0;
    struct xv6fs_inode *ip_new = (struct xv6fs_inode *)0;
    struct xv6fs_dirent de;
    uint32_t off_old = 0, off_new = 0;
    int same_dir, is_dir = 0;
    int ip_locked = 0, ip_new_locked = 0;
    int rc = 0;

    if (!oldpath || !newpath) return -14;              /* EFAULT */

    dp_old = xv6fs_nameiparent(oldpath, old_name);
    if (!dp_old) return -2;                            /* ENOENT */
    dp_new = xv6fs_nameiparent(newpath, new_name);
    if (!dp_new) { xv6fs_iput(dp_old); return -2; }

    /* iget は同じ inum に同じ実体を返すので、ポインタ比較でよい */
    same_dir = (dp_old == dp_new);

    xv6_sleep_lock(&g_rename_lock);
    xv6log_begin_op(XV6LOG_OP_FULL);

    /* **親を 2 つ掴む。**同じなら 1 回だけ (二度掛けると眠ったまま戻らない)。
     * 違うなら inum の小さいほうから取り、rename どうしがすれ違っても
     * 掴む順序が揃うようにする */
    if (same_dir) {
        xv6fs_ilock(dp_old);
    } else if (dp_old->inum < dp_new->inum) {
        xv6fs_ilock(dp_old);
        xv6fs_ilock(dp_new);
    } else {
        xv6fs_ilock(dp_new);
        xv6fs_ilock(dp_old);
    }

    ip = xv6fs_dirlookup(dp_old, old_name, &off_old);
    if (!ip) { rc = -2; goto out; }                    /* ENOENT */
    ip_new = xv6fs_dirlookup(dp_new, new_name, &off_new);

    /* 同じ実体を指す名前どうし。POSIX は「何もせずに成功」 */
    if (ip_new && ip_new->inum == ip->inum) goto out;
    /* 自分の中へ入ろうとしている */
    if (ip->inum == dp_new->inum) { rc = -22; goto out; }          /* EINVAL */
    /* 移動先が移動元の親そのもの。中身が入っているので空ではない */
    if (ip_new && (ip_new->inum == dp_old->inum ||
                   ip_new->inum == dp_new->inum)) {
        rc = -39; goto out;                                        /* ENOTEMPTY */
    }

    xv6fs_ilock(ip); ip_locked = 1;
    is_dir = (ip->type == XV6FS_T_DIR);
    if (ip_new) {
        xv6fs_ilock(ip_new); ip_new_locked = 1;
        if (is_dir && ip_new->type != XV6FS_T_DIR) { rc = -20; goto out; }  /* ENOTDIR */
        if (!is_dir && ip_new->type == XV6FS_T_DIR) { rc = -21; goto out; } /* EISDIR */
        if (ip_new->type == XV6FS_T_DIR && !dir_is_empty(ip_new)) {
            rc = -39; goto out;                                    /* ENOTEMPTY */
        }
    }

    /* --- ここから書く。**伸びる可能性のある操作を先に済ませる。**
     * 置き換えのときは既にあるエントリに上書きするだけで何も足さない。
     * 新規のときだけ dirlink がディレクトリを伸ばしうるので、
     * 古い名前を消す前に済ませる。こうしておけば失敗しても実体は
     * 古い名前から辿れる */
    if (ip_new) {
        memset(de.name, 0, XV6FS_DIRSIZ);
        strncpy(de.name, new_name, XV6FS_DIRSIZ);
        de.inum = (uint16_t)ip->inum;
        if (xv6fs_writei(dp_new, &de, off_new, sizeof(de)) != (int)sizeof(de)) {
            rc = -5; goto out;                                     /* EIO */
        }
    } else if (xv6fs_dirlink(dp_new, new_name, ip->inum) < 0) {
        rc = -28; goto out;                                        /* ENOSPC */
    }

    /* 古い名前を消す */
    memset(&de, 0, sizeof(de));
    xv6fs_writei(dp_old, &de, off_old, sizeof(de));

    /* **実体 (ip) の nlink は変わらない。**名前を 1 つ足して 1 つ消している */
    if (ip_new) {
        if (ip_new->type == XV6FS_T_DIR) {
            /* 消えたディレクトリの ".." のぶん、親が 1 つ減る */
            dp_new->nlink--;
            ip_new->nlink = 0;
        } else {
            ip_new->nlink--;
        }
        xv6fs_iupdate(ip_new);
    }
    if (is_dir && !same_dir) {
        if (dir_reparent(ip, dp_new->inum) < 0) { rc = -5; goto out; }
        dp_old->nlink--;
        dp_new->nlink++;
    }
    xv6fs_iupdate(ip);
    xv6fs_iupdate(dp_old);
    if (!same_dir) xv6fs_iupdate(dp_new);

out:
    /* **iput は眠るので、掛けたロックを外してから呼ぶ。**
     * nlink が 0 になった相手はここでブロックごと解放される */
    if (ip_new_locked) xv6fs_iunlock(ip_new);
    if (ip_new) xv6fs_iput(ip_new);
    if (ip_locked) xv6fs_iunlock(ip);
    if (ip) xv6fs_iput(ip);
    xv6fs_iunlock(dp_old);
    if (!same_dir) xv6fs_iunlock(dp_new);
    xv6log_end_op(XV6LOG_OP_FULL);
    xv6_sleep_unlock(&g_rename_lock);
    /* nameiparent を 2 回通しているので、同じ親でも参照は 2 つ */
    xv6fs_iput(dp_old);
    xv6fs_iput(dp_new);
    return rc;
}

int xv6fs_mkdir_path(const char *path, int mode) {
    char name[XV6FS_DIRSIZ];
    struct xv6fs_inode *dp = xv6fs_nameiparent(path, name);
    if (!dp) return -1;

    xv6log_begin_op(XV6LOG_OP_SMALL);
    xv6fs_ilock(dp);
    struct xv6fs_inode *ip = xv6fs_ialloc(dp->dev, XV6FS_T_DIR);
    if (!ip) {
        xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_SMALL); xv6fs_iput(dp); return -1;
    }
    xv6fs_ilock(ip);
    ip->nlink = 1;
    xv6fs_set_mode(ip, (uint32_t)mode);
    xv6fs_iupdate(ip);
    xv6fs_dirlink(ip, ".", ip->inum);
    xv6fs_dirlink(ip, "..", dp->inum);
    xv6fs_dirlink(dp, name, ip->inum);
    dp->nlink++;
    xv6fs_iupdate(dp);
    xv6fs_iunlock(ip); xv6fs_iput(ip);
    xv6fs_iunlock(dp); xv6log_end_op(XV6LOG_OP_SMALL); xv6fs_iput(dp);
    return 0;
}

int xv6fs_chmod_path(const char *path, uint32_t mode) {
    struct xv6fs_inode *ip = xv6fs_namei(path);
    if (!ip) return -1;
    xv6log_begin_op(XV6LOG_OP_SMALL);
    xv6fs_ilock(ip);
    if (ip->type != XV6FS_T_DIR && ip->type != XV6FS_T_FILE) {
        xv6fs_iunlock(ip);
        xv6log_end_op(XV6LOG_OP_SMALL);
        xv6fs_iput(ip);
        return -1;
    }
    xv6fs_set_mode(ip, mode);
    xv6fs_iupdate(ip);
    xv6fs_iunlock(ip);
    xv6log_end_op(XV6LOG_OP_SMALL);
    xv6fs_iput(ip);
    return 0;
}

/* statfs(2) の材料を集める。**空き容量が見えないまま数時間のビルドを
 * 回すのは危ない** —— 3 時間走らせた末に容量切れで落ちるのが最悪の
 * 失敗の仕方なので、`df` が動くようにする (2026-08-30)。
 *
 * **ビットマップと inode 表を舐める。**balloc / ialloc と同じ歩き方。
 * `df` は人が打つものなので、1 秒前後かかっても構わない
 * (1.5GB の FS でビットマップ 192 ブロック + inode 表 2048 ブロック)。
 *
 * ブロックは 1 つずつ数えるのではなく、**バイト単位でビットを立てて
 * いない数を数える** —— 8 ビットまとめて見るほうが速い。 */
int xv6fs_statfs(struct xv6fs_statfs *out) {
    uint32_t b, i;
    uint64_t bfree = 0, ifree = 0;

    if (!out || !xv6fs_is_mounted()) return -1;

    for (b = 0; b < g_sb.size; b += XV6FS_BPB) {
        struct xv6buf *bp = xv6bread(g_xv6fs_dev, XV6FS_BBLOCK(b, g_sb));
        for (uint32_t bi = 0; bi < XV6FS_BPB && b + bi < g_sb.size; bi++) {
            if ((bp->data[bi / 8] & (1 << (bi % 8))) == 0) bfree++;
        }
        xv6brelse(bp);
    }

    /* inode 表は 1 ブロックに XV6FS_IPB 個。**ブロックごとに 1 回読む** */
    for (i = 1; i < g_sb.ninodes; ) {
        struct xv6buf *bp = xv6bread(g_xv6fs_dev, XV6FS_IBLOCK(i, g_sb));
        struct xv6fs_dinode *dip = (struct xv6fs_dinode *)bp->data;
        uint32_t upto = i + XV6FS_IPB - (i % XV6FS_IPB);
        if (upto > g_sb.ninodes) upto = g_sb.ninodes;
        for (; i < upto; i++) {
            if (dip[i % XV6FS_IPB].type == 0) ifree++;
        }
        xv6brelse(bp);
    }

    out->bsize   = XV6FS_BSIZE;
    out->blocks  = g_sb.size;
    out->bfree   = bfree;
    out->files   = g_sb.ninodes;
    out->ffree   = ifree;
    out->namelen = XV6FS_DIRSIZ;
    return 0;
}

int xv6fs_sync(void) {
    /* **P-6 以前はここが空でよかった** —— end_op が毎回コミットしていたから。
     * 溜めるようにしたので、**ここが唯一の「必ず出す」約束**になった */
    xv6log_flush();
    return 0;
}
