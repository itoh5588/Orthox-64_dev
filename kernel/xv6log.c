/*
 * Portions of this file are derived from xv6-riscv (MIT License):
 *   Copyright (c) 2006-2019 Frans Kaashoek, Robert Morris, Russ Cox,
 *                           Massachusetts Institute of Technology
 * The full xv6 MIT license text is reproduced in THIRD_PARTY_NOTICES.md.
 */
/*
 * xv6log.c — ジャーナリング層
 * xv6-riscv/kernel/log.c を Orthox-64 向けに移植。
 *
 * 変更点:
 *   struct proc / myproc() / sleep() / wakeup() を除去
 *   sleep 待機 → spinlock + スピン待機で代替
 *   initlock / acquire / release → spinlock_init / spin_lock / spin_unlock
 *   bread/bwrite/brelse/bpin/bunpin → xv6 プレフィックス版
 *   BSIZE / LOGBLOCKS → XV6FS_BSIZE / XV6FS_LOGBLOCKS
 */

#include "xv6fs.h"
#include "kassert.h"
#include <stdarg.h>

extern int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap);
extern int64_t sys_write_serial(const char *buf, size_t count);
extern void *memcpy(void *dst, const void *src, size_t n);

static void xv6log_print(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sys_write_serial(buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* on-disk ログヘッダ                                                  */
/* ------------------------------------------------------------------ */

struct logheader {
    int n;
    int block[XV6FS_LOGBLOCKS];
};

/* ------------------------------------------------------------------ */
/* in-memory ログ状態                                                  */
/* ------------------------------------------------------------------ */

struct xv6log {
    spinlock_t lock;
    int        start;        /* logstart ブロック番号 */
    int        outstanding;  /* 進行中の FS システムコール数 */
    int        committing;   /* commit() 実行中フラグ */
    uint32_t   dev;
    struct logheader lh;
};

static struct xv6log lg;

/* begin_op がログ空きを待つための待機列。
 * 状態 (lg.*) は lg.lock が保護し、状態変更後に wake する。 */
static struct wait_queue lg_wait;

static void commit(void);
static void recover_from_log(void);

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void xv6log_init(uint32_t dev, struct xv6fs_superblock *sb) {
    KASSERT(sizeof(struct logheader) < XV6FS_BSIZE);

    spinlock_init(&lg.lock);
    wait_queue_init(&lg_wait);
    lg.start       = (int)sb->logstart;
    lg.dev         = dev;
    lg.outstanding = 0;
    lg.committing  = 0;
    lg.lh.n        = 0;

    recover_from_log();
}

/* ------------------------------------------------------------------ */
/* ログブロックをホーム位置にコピー                                    */
/* ------------------------------------------------------------------ */

static void install_trans(int recovering) {
    for (int tail = 0; tail < lg.lh.n; tail++) {
        if (recovering)
            xv6log_print("xv6log: recover tail=%d dst=%d\n",
                         tail, lg.lh.block[tail]);
        struct xv6buf *lbuf = xv6bread(lg.dev, (uint32_t)(lg.start + tail + 1));
        struct xv6buf *dbuf = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
        memcpy(dbuf->data, lbuf->data, XV6FS_BSIZE);
        xv6bwrite(dbuf);
        if (!recovering)
            xv6bunpin(dbuf);
        xv6brelse(lbuf);
        xv6brelse(dbuf);
    }
}

/* ------------------------------------------------------------------ */
/* ログヘッダ読み書き                                                  */
/* ------------------------------------------------------------------ */

static void read_head(void) {
    struct xv6buf *buf = xv6bread(lg.dev, (uint32_t)lg.start);
    struct logheader *lh = (struct logheader *)buf->data;
    lg.lh.n = lh->n;
    for (int i = 0; i < lg.lh.n; i++)
        lg.lh.block[i] = lh->block[i];
    xv6brelse(buf);
}

static void write_head(void) {
    struct xv6buf *buf = xv6bread(lg.dev, (uint32_t)lg.start);
    struct logheader *hb = (struct logheader *)buf->data;
    hb->n = lg.lh.n;
    for (int i = 0; i < lg.lh.n; i++)
        hb->block[i] = lg.lh.block[i];
    xv6bwrite(buf);
    xv6brelse(buf);
}

/* ------------------------------------------------------------------ */
/* クラッシュリカバリ                                                  */
/* ------------------------------------------------------------------ */

void xv6log_recover(void) {
    recover_from_log();
}

static void recover_from_log(void) {
    read_head();
    install_trans(1);
    lg.lh.n = 0;
    write_head();
}

/* ------------------------------------------------------------------ */
/* トランザクション開始 / 終了                                         */
/* ------------------------------------------------------------------ */

/* begin_op が進めそうかのヒント (正確な判定は lg.lock 下で再確認する)。 */
static int xv6log_progress_hint(void *arg) {
    (void)arg;
    return !lg.committing &&
           lg.lh.n + (lg.outstanding + 1) * 10 <= XV6FS_LOGBLOCKS;
}

void xv6log_begin_op(void) {
    for (;;) {
        spin_lock(&lg.lock);
        if (!lg.committing &&
            lg.lh.n + (lg.outstanding + 1) * 10 <= XV6FS_LOGBLOCKS) {
            lg.outstanding++;
            spin_unlock(&lg.lock);
            return;
        }
        spin_unlock(&lg.lock);
        /* ログが満杯 or commit 中: 待機列で眠る (end_op / commit 完了で wake) */
        wait_event(&lg_wait, xv6log_progress_hint, 0);
    }
}

void xv6log_end_op(void) {
    int do_commit = 0;

    spin_lock(&lg.lock);
    lg.outstanding--;
    KASSERT(!lg.committing);
    if (lg.outstanding == 0) {
        do_commit = 1;
        lg.committing = 1;
    }
    spin_unlock(&lg.lock);

    if (do_commit) {
        commit();
        spin_lock(&lg.lock);
        lg.committing = 0;
        spin_unlock(&lg.lock);
    }
    /* outstanding 減少 or commit 完了でログ空きが変化した */
    wake_up_all(&lg_wait);
}

/* ------------------------------------------------------------------ */
/* キャッシュ → ログへの書き込み                                       */
/* ------------------------------------------------------------------ */

static void write_log(void) {
    for (int tail = 0; tail < lg.lh.n; tail++) {
        struct xv6buf *to   = xv6bread(lg.dev, (uint32_t)(lg.start + tail + 1));
        struct xv6buf *from = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
        memcpy(to->data, from->data, XV6FS_BSIZE);
        xv6bwrite(to);
        xv6brelse(from);
        xv6brelse(to);
    }
}

static void commit(void) {
    if (lg.lh.n > 0) {
        write_log();
        write_head();
        install_trans(0);
        lg.lh.n = 0;
        write_head();
    }
}

/* ------------------------------------------------------------------ */
/* ブロックをログに登録する                                            */
/* ------------------------------------------------------------------ */

void xv6log_write(struct xv6buf *b) {
    int i;

    spin_lock(&lg.lock);
    KASSERT(lg.lh.n < XV6FS_LOGBLOCKS);
    KASSERT(lg.outstanding >= 1);

    for (i = 0; i < lg.lh.n; i++) {
        if (lg.lh.block[i] == (int)b->blockno)
            break;   /* log absorption: 同一ブロックは1エントリ */
    }
    lg.lh.block[i] = (int)b->blockno;
    if (i == lg.lh.n) {
        xv6bpin(b);
        lg.lh.n++;
    }
    spin_unlock(&lg.lock);
}
