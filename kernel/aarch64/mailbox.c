/*
 * VideoCore の mailbox (Raspberry Pi)。property interface だけを扱う。
 *
 * ---- 出典 ----------------------------------------------------------------
 *
 * **仕様から起こした。**他所のコードは持ち込んでいない (このリポジトリは
 * MIT で、Linux の bcm2835-mailbox は GPL-2.0 のため)。根拠にしたのは
 * Raspberry Pi 公式 wiki の記述:
 *
 *   Mailboxes                  レジスタ配置、MB0/MB1 の役割
 *   Accessing mailboxes        STATUS のビット、語の詰め方、手順
 *   Mailbox property interface  バッファの形、タグ番号
 *
 * ---- なぜ mailbox が要るか -------------------------------------------------
 *
 * **Pi の画面は DTB からは取れない。**解像度もフレームバッファの番地も
 * ファームウェア (VideoCore) が握っていて、聞く口がここしかない。
 *
 * emmc2.c は「EMMC2 は素直な SDHCI なので mailbox を使わずに済む」と
 * 判断して意図的に避けたが (日報2026-08-15 §4)、**画面には迂回路が無い。**
 *
 * ---- 注意した点 ------------------------------------------------------------
 *
 * **ARM は MB0 を書かず、MB1 を読まない。**公式にそう書いてある:
 *
 *   Only mailbox 0's status can trigger interrupts on the ARM, so MB 0 is
 *   always for communication from VC to ARM and MB 1 is for ARM to VC.
 *   The ARM should never write MB 0 or read MB 1.
 *
 * したがって**送信で見るのは MB1 の STATUS (+0x38)**、受信で見るのは
 * MB0 の STATUS (+0x18)。よく出回っている実装は送信でも +0x18 を見ている
 * が、それは MB0 (自分が読む側) の空き具合であって、送信の可否ではない。
 *
 * **MMU を入れる前に呼ぶ。**キャッシュがまだ効いていない (SCTLR.C = 0) ので、
 * バッファへの書き込みがそのまま DRAM に載り、VC から見える。MMU を入れた
 * 後に呼ぶなら、バッファのクリーン/無効化が別途要る。
 */
#include <stdint.h>
#include "aarch64/boot.h"

/* Pi 4 (BCM2711) の既定値。DTB から取れれば上書きする。
 * 周辺の基底が 0xfe000000 で、mailbox はそこから +0xb880 */
#define AARCH64_MBOX_BASE_DEFAULT   0xfe00b880ULL

#define MBOX_MB0_READ    0x00U    /* VC -> ARM。**ARM はここを読む** */
#define MBOX_MB0_STATUS  0x18U
#define MBOX_MB1_WRITE   0x20U    /* ARM -> VC。**ARM はここへ書く** */
#define MBOX_MB1_STATUS  0x38U

#define MBOX_STATUS_FULL   0x80000000U   /* 書く空きが無い */
#define MBOX_STATUS_EMPTY  0x40000000U   /* 読むものが無い */

#define MBOX_CHANNEL_MASK  0x0000000fU

/* **待ちは必ず有限にする。** VC が黙ったときに永久に回ると、
 * 「画面が出ない」ではなく「起動しない」になり、切り分けが難しくなる。
 * 実測では property 1 往復が 1 万回転もかからない */
#define MBOX_SPIN_LIMIT    100000000U

static uint64_t g_mbox_base;

static inline uint32_t mbox_r32(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(g_mbox_base + off);
}
static inline void mbox_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(g_mbox_base + off) = v;
}

void aarch64_mbox_set_base(uint64_t base) { g_mbox_base = base; }

uint64_t aarch64_mbox_base(void) { return g_mbox_base; }

/* DTB から取れていればそれを、無ければ Pi 4 の既定値を使う。
 * **QEMU virt には mailbox が無い**ので、そこでは 0 のままにして
 * 「この機械には画面が無い」と分かる形にする */
void aarch64_mbox_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    if (b && b->mbox_base != 0) {
        g_mbox_base = b->mbox_base;
        return;
    }
    /* DTB に無い。**Pi かどうかは emmc2 の有無で見分ける** — Pi 4 の DTB は
     * mailbox も emmc2 も持っているので、emmc2 が取れていて mailbox だけ
     * 取れないなら DTB の読み落としであって、機械に無いわけではない */
    if (b && b->emmc2_base != 0) {
        g_mbox_base = AARCH64_MBOX_BASE_DEFAULT;
        return;
    }
    g_mbox_base = 0;
}

/* property のやり取りを 1 往復。
 *
 * buf は **16 バイト境界**に置くこと (下位 4 ビットをチャネルに使うため)。
 * buf[0] にバイト数、buf[1] に 0 を入れてから呼ぶと、
 * 戻りで buf[1] が 0x80000000 になる。
 *
 * 戻り値: 0 = 成功、負 = 失敗 (-1 番地なし / -2 送信待ちで力尽きた /
 *         -3 受信待ちで力尽きた / -4 VC が失敗を返した) */
int aarch64_mbox_property(volatile uint32_t* buf) {
    uint32_t spins;
    uint64_t pa;
    uint32_t word;

    if (g_mbox_base == 0 || buf == 0) return -1;

    pa = (uint64_t)(uintptr_t)buf;

    /* **VC に渡すのはバス番地。** ARM の物理に 0xc0000000 を被せた別名が
     * 「L2 を通さない」眺めで、ファームウェアはこちらを期待する。
     * 下位 4 ビットはチャネルに使うので 0 でなければならない */
    word = (uint32_t)((pa | 0xc0000000ULL) & 0xfffffff0ULL) | 8U;

    /* 送信: MB1 が満杯でなくなるまで待つ */
    for (spins = 0; (mbox_r32(MBOX_MB1_STATUS) & MBOX_STATUS_FULL) != 0; spins++) {
        if (spins >= MBOX_SPIN_LIMIT) return -2;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    mbox_w32(MBOX_MB1_WRITE, word);

    /* 受信: MB0 に何か来るまで待ち、**チャネルが 8 のものだけ**拾う。
     * 別のチャネルの返事が先に来ることがあるので捨てて待ち直す */
    for (spins = 0; ; spins++) {
        uint32_t got;
        if (spins >= MBOX_SPIN_LIMIT) return -3;
        if ((mbox_r32(MBOX_MB0_STATUS) & MBOX_STATUS_EMPTY) != 0) continue;
        got = mbox_r32(MBOX_MB0_READ);
        if ((got & MBOX_CHANNEL_MASK) != 8U) continue;
        break;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    /* **「返事が来た」と「成功した」は別。** 応答コードを見ないと、
     * タグが 1 つも通っていなくても成功に見える */
    if (buf[1] != 0x80000000U) return -4;
    return 0;
}
