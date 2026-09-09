/*
 * Raspberry Pi 4 (BCM2711) の実 NIC — Broadcom GENETv5 (N-9 手2)。
 *
 * **土台は OpenBSD/NetBSD 共通の `sys/dev/ic/bcmgenet.c`
 * (BSD-2-Clause、Copyright (c) 2020 Jared McNeill, Mark Kettenis)。**
 * レジスタ配置・リセット手順・DMA リングの組み方をそちらから取っている。
 * 著作権表示は THIRD_PARTY_NOTICES.md を参照 (kernel/aarch64/emmc2.c の
 * rpi-boot 由来の扱いと同じ形)。
 *
 * ---- 変えた点 --------------------------------------------------------
 *
 *   1. mbuf/bus_dma を使わない。**バッファは固定 32 本 (RX) / 8 本 (TX) の
 *      物理ページ**で、記述子のアドレスは初期化時に一度だけ書いて使い回す
 *      (virtio_net_mmio.c と同じ設計)。フラグメント (scatter-gather) は
 *      要らない — 1 パケット 1 バッファで足りる
 *   2. **MDIO は IEEE 802.3 clause 22 の標準レジスタだけ使う。**
 *      BCM54213PE 固有のレジスタは読まない (データシートが手元に無い)。
 *      自動ネゴシエーションを回し、解決結果は ANAR/ANLPAR (10/100) と
 *      GBCR/GBSR (1000) の標準的な優先順位で自分で組み立てる
 *   3. **ポーリングのみ。**virtio-net-mmio と同じ理由 (割り込み経路が
 *      まだ無い、DHCP/ping が動くかを先に見る)
 *
 * ---- なぜ PHY リセットをしない ----------------------------------------
 *
 * **Pi 4 のブートローダ (EEPROM の recovery.bin / start4.elf) は、まさに
 * この GENET+PHY を使って TFTP でファイルを取ってくる**
 * (scripts/pi4/README.md の netboot)。つまり我々のカーネルが動き出す
 * 時点で、物理リンクは既に上がっている可能性が高い。
 * `genet_reset()` (このファイルの `genet_mac_reset`) は GENET の MAC/DMA
 * 側だけをリセットし、MDIO 経由で PHY に触らない — 反対に自動
 * ネゴシエーションを明示的に再起動すると、それまで上がっていたリンクが
 * 数秒落ちる。**確実性を優先し、起動のたびに正規の手順でネゴシエーション
 * し直す**(数秒待つ)。
 *
 * ---- 実機でしか確かめられない -------------------------------------------
 *
 * QEMU virt にはこのハードウェアが無い (DTB に compatible ノードが無い)。
 * QEMU raspi4b に実物の DTB を渡しても、QEMU 側に GENET の実装は無いので
 * 動作は確かめられない — 実機での確認が必須 (2026-09-04)。
 */
#include <stdint.h>
#include <stddef.h>
#include "virtio_net.h"
#include "spinlock.h"
#include "aarch64/boot.h"
#include "aarch64/vm.h"

uint64_t aarch64_pmm_alloc(uint64_t pages);
void aarch64_gic_enable_irq(unsigned intid);
void aarch64_uart_puts(const char* s);
void aarch64_uart_putchar(char c);
uint64_t arch_time_now_ms(void);

static void put(const char* s) { aarch64_uart_puts(s); }

static void putdec(uint64_t v) {
    char buf[21];
    int i = 0;
    if (v == 0) { put("0"); return; }
    while (v) { buf[i++] = (char)('0' + (uint32_t)(v % 10U)); v /= 10U; }
    while (i--) aarch64_uart_putchar(buf[i]);
}

static void puthex_bare(uint64_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) aarch64_uart_putchar(d[(v >> (i * 4)) & 0xf]);
}

/* ---- レジスタ (dev/ic/bcmgenetreg.h 由来) --------------------------------- */

#define GENET_SYS_REV_CTRL              0x000
#define GENET_SYS_PORT_CTRL             0x004
#define  GENET_SYS_PORT_MODE_EXT_GPHY   3
#define GENET_SYS_RBUF_FLUSH_CTRL       0x008
#define  GENET_SYS_RBUF_FLUSH_RESET     (1U << 1)
#define GENET_EXT_RGMII_OOB_CTRL        0x08c
#define  GENET_EXT_RGMII_OOB_ID_MODE_DISABLE (1U << 16)
#define  GENET_EXT_RGMII_OOB_RGMII_MODE_EN   (1U << 6)
#define  GENET_EXT_RGMII_OOB_OOB_DISABLE     (1U << 5)
#define  GENET_EXT_RGMII_OOB_RGMII_LINK      (1U << 4)
#define GENET_RBUF_CTRL                 0x300
#define  GENET_RBUF_ALIGN_2B            (1U << 1)
#define GENET_RBUF_TBUF_SIZE_CTRL       0x3b4
#define GENET_UMAC_CMD                  0x808
#define  GENET_UMAC_CMD_LCL_LOOP_EN     (1U << 15)
#define  GENET_UMAC_CMD_SW_RESET        (1U << 13)
#define  GENET_UMAC_CMD_PROMISC         (1U << 4)
#define  GENET_UMAC_CMD_SPEED_SHIFT     2
#define  GENET_UMAC_CMD_SPEED_MASK      (3U << GENET_UMAC_CMD_SPEED_SHIFT)
#define   GENET_UMAC_CMD_SPEED_10       0U
#define   GENET_UMAC_CMD_SPEED_100      1U
#define   GENET_UMAC_CMD_SPEED_1000     2U
#define  GENET_UMAC_CMD_RXEN            (1U << 1)
#define  GENET_UMAC_CMD_TXEN            (1U << 0)
#define GENET_UMAC_MAC0                 0x80c
#define GENET_UMAC_MAC1                 0x810
#define GENET_UMAC_MAX_FRAME_LEN        0x814
#define GENET_UMAC_TX_FLUSH             0xb34
#define GENET_UMAC_MIB_CTRL             0xd80
#define  GENET_UMAC_MIB_RESET_TX        (1U << 2)
#define  GENET_UMAC_MIB_RESET_RUNT      (1U << 1)
#define  GENET_UMAC_MIB_RESET_RX        (1U << 0)
#define GENET_MDIO_CMD                  0xe14
#define  GENET_MDIO_START_BUSY          (1U << 29)
#define  GENET_MDIO_READ                (1U << 27)
#define  GENET_MDIO_WRITE               (1U << 26)
#define  GENET_MDIO_PMD_SHIFT           21
#define  GENET_MDIO_REG_SHIFT           16
/* ---- INTRL2_0 (N-11, 2026-09-04) ----------------------------------------
 *
 * **並びは 0x200 から 4 バイト刻み。** STAT (0x200) と CLEAR (0x208) は
 * 移植したときから合っており、その 2 つが一致することを手掛かりに残りを
 * 埋めた。Linux の bcmgenet と NetBSD/OpenBSD の bcmgenet で同じ並び・
 * 同じビット位置になっている (データシートは手元に無いので、**実機で
 * 割り込みが実際に来るかを確かめてから本採用する**。来なくても
 * ポーリングで動くように残してある) */
#define GENET_INTRL2_CPU_STAT           0x200
#define GENET_INTRL2_CPU_SET            0x204
#define GENET_INTRL2_CPU_CLEAR          0x208
#define GENET_INTRL2_CPU_MASK_STATUS    0x20c
#define GENET_INTRL2_CPU_MASK_SET       0x210
#define GENET_INTRL2_CPU_MASK_CLEAR     0x214
#define  GENET_IRQ_RXDMA_DONE           (1U << 13)
#define  GENET_IRQ_TXDMA_DONE           (1U << 16)
#define GENET_UMAC_MDF_CTRL             0xe50
#define GENET_UMAC_MDF_ADDR0(n)         (0xe54U + (uint32_t)(n) * 0x8U)
#define GENET_UMAC_MDF_ADDR1(n)         (0xe58U + (uint32_t)(n) * 0x8U)

#define GENET_DMA_DESC_SIZE             12U
#define GENET_DMA_DEFAULT_QUEUE         16U   /* ハード固定。変えない */

#define GENET_RX_BASE                   0x2000U
#define GENET_TX_BASE                   0x4000U

#define GENET_RX_DMA_RINGBASE(q)        (GENET_RX_BASE + 0xc00U + 0x40U * (q))
#define GENET_RX_DMA_WRITE_PTR_LO(q)    (GENET_RX_DMA_RINGBASE(q) + 0x00U)
#define GENET_RX_DMA_WRITE_PTR_HI(q)    (GENET_RX_DMA_RINGBASE(q) + 0x04U)
#define GENET_RX_DMA_PROD_INDEX(q)      (GENET_RX_DMA_RINGBASE(q) + 0x08U)
#define GENET_RX_DMA_CONS_INDEX(q)      (GENET_RX_DMA_RINGBASE(q) + 0x0cU)
#define GENET_RX_DMA_RING_BUF_SIZE(q)   (GENET_RX_DMA_RINGBASE(q) + 0x10U)
#define GENET_RX_DMA_START_ADDR_LO(q)   (GENET_RX_DMA_RINGBASE(q) + 0x14U)
#define GENET_RX_DMA_START_ADDR_HI(q)   (GENET_RX_DMA_RINGBASE(q) + 0x18U)
#define GENET_RX_DMA_END_ADDR_LO(q)     (GENET_RX_DMA_RINGBASE(q) + 0x1cU)
#define GENET_RX_DMA_END_ADDR_HI(q)     (GENET_RX_DMA_RINGBASE(q) + 0x20U)
#define GENET_RX_DMA_XON_XOFF_THRES(q)  (GENET_RX_DMA_RINGBASE(q) + 0x28U)
#define GENET_RX_DMA_READ_PTR_LO(q)     (GENET_RX_DMA_RINGBASE(q) + 0x2cU)
#define GENET_RX_DMA_READ_PTR_HI(q)     (GENET_RX_DMA_RINGBASE(q) + 0x30U)

#define GENET_TX_DMA_RINGBASE(q)        (GENET_TX_BASE + 0xc00U + 0x40U * (q))
#define GENET_TX_DMA_READ_PTR_LO(q)     (GENET_TX_DMA_RINGBASE(q) + 0x00U)
#define GENET_TX_DMA_READ_PTR_HI(q)     (GENET_TX_DMA_RINGBASE(q) + 0x04U)
#define GENET_TX_DMA_CONS_INDEX(q)      (GENET_TX_DMA_RINGBASE(q) + 0x08U)
#define GENET_TX_DMA_PROD_INDEX(q)      (GENET_TX_DMA_RINGBASE(q) + 0x0cU)
#define GENET_TX_DMA_RING_BUF_SIZE(q)   (GENET_TX_DMA_RINGBASE(q) + 0x10U)
#define GENET_TX_DMA_START_ADDR_LO(q)   (GENET_TX_DMA_RINGBASE(q) + 0x14U)
#define GENET_TX_DMA_START_ADDR_HI(q)   (GENET_TX_DMA_RINGBASE(q) + 0x18U)
#define GENET_TX_DMA_END_ADDR_LO(q)     (GENET_TX_DMA_RINGBASE(q) + 0x1cU)
#define GENET_TX_DMA_END_ADDR_HI(q)     (GENET_TX_DMA_RINGBASE(q) + 0x20U)
#define GENET_TX_DMA_MBUF_DONE_THRES(q) (GENET_TX_DMA_RINGBASE(q) + 0x24U)
#define GENET_TX_DMA_FLOW_PERIOD(q)     (GENET_TX_DMA_RINGBASE(q) + 0x28U)
#define GENET_TX_DMA_WRITE_PTR_LO(q)    (GENET_TX_DMA_RINGBASE(q) + 0x2cU)
#define GENET_TX_DMA_WRITE_PTR_HI(q)    (GENET_TX_DMA_RINGBASE(q) + 0x30U)

#define GENET_RX_DESC_STATUS(i)     (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x00U)
#define  GENET_RX_DESC_STATUS_BUFLEN_SHIFT 16
#define GENET_RX_DESC_ADDRESS_LO(i) (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x04U)
#define GENET_RX_DESC_ADDRESS_HI(i) (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x08U)

#define GENET_TX_DESC_STATUS(i)     (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x00U)
#define  GENET_TX_DESC_STATUS_BUFLEN_SHIFT 16
#define  GENET_TX_DESC_STATUS_OWN   (1U << 15)
#define  GENET_TX_DESC_STATUS_EOP   (1U << 14)
#define  GENET_TX_DESC_STATUS_SOP   (1U << 13)
#define  GENET_TX_DESC_STATUS_QTAG  (0x3fU << 7)   /* bcmgenetreg.h: __BITS(12,7) = 6 bit */
#define  GENET_TX_DESC_STATUS_CRC   (1U << 6)
#define GENET_TX_DESC_ADDRESS_LO(i) (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x04U)
#define GENET_TX_DESC_ADDRESS_HI(i) (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (i) + 0x08U)

#define GENET_RX_DMA_RING_CFG   (GENET_RX_BASE + 0x1040U + 0x00U)
#define GENET_RX_DMA_CTRL       (GENET_RX_BASE + 0x1040U + 0x04U)
#define  GENET_RX_DMA_CTRL_RBUF_EN(q) (1U << ((q) + 1))
#define  GENET_RX_DMA_CTRL_EN         (1U << 0)
#define GENET_RX_SCB_BURST_SIZE (GENET_RX_BASE + 0x1040U + 0x0cU)

#define GENET_TX_DMA_RING_CFG   (GENET_TX_BASE + 0x1040U + 0x00U)
#define GENET_TX_DMA_CTRL       (GENET_TX_BASE + 0x1040U + 0x04U)
#define  GENET_TX_DMA_CTRL_RBUF_EN(q) (1U << ((q) + 1))
#define  GENET_TX_DMA_CTRL_EN         (1U << 0)
#define GENET_TX_SCB_BURST_SIZE (GENET_TX_BASE + 0x1040U + 0x0cU)

/* ---- MDIO (IEEE 802.3 clause 22 標準レジスタのみ) ------------------------ */
#define MII_BMCR        0x00
#define  BMCR_RESET     (1U << 15)
#define  BMCR_SPEED1000 (1U << 6)
#define  BMCR_AUTOEN    (1U << 12)
#define  BMCR_STARTNEG  (1U << 9)
#define  BMCR_FDX       (1U << 8)
#define MII_BMSR        0x01
#define  BMSR_ANEGCOMPLETE (1U << 5)
#define  BMSR_LINK         (1U << 2)
#define MII_ANAR        0x04
#define  ANAR_10        (1U << 5)
#define  ANAR_10_FD     (1U << 6)
#define  ANAR_TX        (1U << 7)
#define  ANAR_TX_FD     (1U << 8)
#define  ANAR_CSMA_SEL  0x0001U
#define MII_ANLPAR      0x05
#define MII_GBCR        0x09   /* 1000BASE-T control */
#define  GBCR_1000T_HD  (1U << 8)
#define  GBCR_1000T_FD  (1U << 9)
#define MII_GBSR        0x0a   /* 1000BASE-T status */
#define  GBSR_LP_1000T_HD (1U << 10)
#define  GBSR_LP_1000T_FD (1U << 11)

/* ---- 状態 ----------------------------------------------------------------- */

#define GENET_RX_RING_COUNT 32U     /* 2 のべき乗。COUNT<=256 (レジスタの都合) */
#define GENET_TX_RING_COUNT 8U
#define GENET_RX_BUF_SIZE   2048U
#define GENET_FRAME_MAX     1518U   /* MAX_FRAME_LEN に合わせる (1536 の余裕内) */
#define MDIO_BUSY_RETRY     1000
#define AUTONEG_WAIT_TICKS_MS 4000U /* 4 秒。リンクが既に上がっていれば即完了する */

static volatile uint8_t* g_base;
static uint64_t g_base_pa;
static uint64_t g_dma_offset;   /* DTB の dma-ranges から。実測は 0 */
static uint32_t g_phy_addr;     /* DTB の mdio 子ノードから。無ければ 1 */
static uint32_t g_phy_mode;     /* 0=rgmii 1=rgmii-id 2=rgmii-rxid 3=rgmii-txid */

static uint8_t* g_rx_bufs[GENET_RX_RING_COUNT];
static uint64_t g_rx_buf_phys[GENET_RX_RING_COUNT];
static uint32_t g_rx_next;      /* 次に見る記述子番号 (0..COUNT-1) */
static uint32_t g_rx_pidx;      /* 直前に読んだ PROD_INDEX (16bit 相当) */

static uint8_t* g_tx_bufs[GENET_TX_RING_COUNT];
static uint64_t g_tx_buf_phys[GENET_TX_RING_COUNT];
static uint32_t g_tx_pidx;      /* 次に使う送信スロット (単調増加、mod で folding) */
static uint32_t g_tx_cidx;

static uint8_t g_mac[6];
static int g_ready = 0;

/* ---- RX の取りこぼしを測る計器 (N-11, 2026-09-04) ------------------------
 *
 * **既定では持たない (AARCH64_VERBOSE_DIAG)。** 60 秒で 6 行出るので、
 * ash で作業しているときには邪魔になる (2026-09-04 §7 と同じ理由)。
 * 取りこぼしを疑ったときだけ
 *   make aarch64-pi4-boot AARCH64_VERBOSE_DIAG=1
 * で出す。
 *
 * 見方: **poll はタイマ (100Hz) と GENET の割り込みの両方から呼ばれる。**
 * RX リングは 32 枚しかないので、拾う前に 32 枚を超えて届くと
 * ハードウェアが古い記述子を上書きしてこちらは取りこぼす。もとの
 * コードは `if (total > RING_COUNT) total = RING_COUNT;` と切り詰める
 * だけで超えた事実を捨てていたので、そこを数えるようにした。
 *
 * 実測 (2026-09-04、実機 60 秒):
 *   ポーリングのみ   maxbatch=14 full=0 over=0   <- 余裕は 2.3 倍しかない
 *   割り込みあり     maxbatch=32 full=1 over=0   <- 32 は起動直後の 1 回だけ
 * どちらも取りこぼしは 0 ([[measure-before-optimizing]]) */
#ifdef AARCH64_VERBOSE_DIAG
static uint64_t g_rx_polls;      /* poll が呼ばれた回数 */
static uint64_t g_rx_frames;     /* 拾ったフレームの累計 */
static uint32_t g_rx_max_batch;  /* 1 回の poll で届いていた最大枚数 */
static uint64_t g_rx_full;       /* ちょうど 32 枚 = 満杯だった回数 (危ない) */
static uint64_t g_rx_over;       /* 32 枚を超えていた回数 = 確実に取りこぼした */
static uint64_t g_rx_lost;       /* 取りこぼした枚数の推定 */
#endif

/* ---- 割り込み (N-11) -----------------------------------------------------
 * g_intid が 0 なら「DTB に番号が無い / まだ開けていない」で、その場合は
 * 従来どおりタイマからのポーリングだけで動く。
 * **回数は既定でも数える** (計器と違って 1 行も出さないので邪魔にならず、
 * 「割り込みが来ているか」は真っ先に知りたい)。
 * aarch64_genet_irq_count() で読める */
static uint32_t g_intid;
static uint64_t g_irq_count;
static uint64_t g_irq_rx;
static uint64_t g_irq_tx;
static virtio_net_rx_cb_t g_rx_cb = 0;
static spinlock_t g_genet_lock;

static void mmio_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(g_base + off) = v;
}
static uint32_t mmio_r32(uint32_t off) {
    return *(volatile uint32_t*)(g_base + off);
}

static void genet_memset(void* p, uint8_t v, uint64_t n) {
    uint8_t* d = (uint8_t*)p;
    while (n--) *d++ = v;
}

/* DMA に渡す物理アドレス。**dma-ranges の offset を足す** (実測は 0 だが、
 * 直書きしない。boot.c/dtb.c の他のデバイスと同じ約束) */
static uint64_t genet_dma_addr(uint64_t phys) {
    return phys + g_dma_offset;
}

/* ---- MDIO ------------------------------------------------------------- */

static uint32_t mdio_read(uint32_t reg) {
    int retry;
    mmio_w32(GENET_MDIO_CMD,
             GENET_MDIO_READ | GENET_MDIO_START_BUSY |
             (g_phy_addr << GENET_MDIO_PMD_SHIFT) |
             (reg << GENET_MDIO_REG_SHIFT));
    for (retry = MDIO_BUSY_RETRY; retry > 0; retry--) {
        if ((mmio_r32(GENET_MDIO_CMD) & GENET_MDIO_START_BUSY) == 0) {
            return mmio_r32(GENET_MDIO_CMD) & 0xffffU;
        }
    }
    return 0xffffU;   /* タイムアウト。0xffff は「応答なし」の慣例値 */
}

static void mdio_write(uint32_t reg, uint32_t val) {
    int retry;
    mmio_w32(GENET_MDIO_CMD,
             (val & 0xffffU) | GENET_MDIO_WRITE | GENET_MDIO_START_BUSY |
             (g_phy_addr << GENET_MDIO_PMD_SHIFT) |
             (reg << GENET_MDIO_REG_SHIFT));
    for (retry = MDIO_BUSY_RETRY; retry > 0; retry--) {
        if ((mmio_r32(GENET_MDIO_CMD) & GENET_MDIO_START_BUSY) == 0) return;
    }
}

/* 自動ネゴシエーションを (再) 起動して結果を待つ。**PHY のハード
 * リセットはしない** (冒頭のコメント参照)。標準レジスタだけで解決結果
 * (speed/duplex) を組み立てる。戻り値: 1=リンク確立, 0=未確立 (続行はする —
 * 何も繋がっていないケーブルでも初期化自体は失敗にしない) */
static int genet_phy_autoneg(uint32_t* out_speed, int* out_fdx) {
    uint32_t bmsr;
    uint64_t deadline;

    /* こちらの能力を表明: 10/100 全部 + 1000BASE-T Full/Half */
    mdio_write(MII_ANAR, ANAR_CSMA_SEL | ANAR_10 | ANAR_10_FD | ANAR_TX | ANAR_TX_FD);
    mdio_write(MII_GBCR, GBCR_1000T_HD | GBCR_1000T_FD);
    mdio_write(MII_BMCR, BMCR_AUTOEN | BMCR_STARTNEG);

    /* **リンクが既に上がっていれば、ここはほぼ即座に完了する**
     * (Pi 4 のブートローダが netboot で先に上げている場合)。
     * 上がっていなければ最大 4 秒待つ */
    deadline = arch_time_now_ms() + AUTONEG_WAIT_TICKS_MS;
    for (;;) {
        bmsr = mdio_read(MII_BMSR);
        if (bmsr & BMSR_ANEGCOMPLETE) break;
        if (arch_time_now_ms() > deadline) break;
    }

    bmsr = mdio_read(MII_BMSR);
    if (!(bmsr & BMSR_LINK)) {
        *out_speed = GENET_UMAC_CMD_SPEED_1000;
        *out_fdx = 1;
        return 0;
    }

    /* **優先順位は 1000 Full > 1000 Half > 100 Full > 100 Half > 10 Full >
     * 10 Half** (IEEE 802.3 clause 28 の標準的な解決順)。
     * 1000BASE-T は双方の GBCR/GBSR、10/100 は ANAR/ANLPAR の共通ビットで見る */
    {
        uint32_t gbsr = mdio_read(MII_GBSR);
        uint32_t anar = mdio_read(MII_ANAR);
        uint32_t anlpar = mdio_read(MII_ANLPAR);
        uint32_t common = anar & anlpar;

        if ((gbsr & GBSR_LP_1000T_FD)) { *out_speed = GENET_UMAC_CMD_SPEED_1000; *out_fdx = 1; }
        else if ((gbsr & GBSR_LP_1000T_HD)) { *out_speed = GENET_UMAC_CMD_SPEED_1000; *out_fdx = 0; }
        else if (common & ANAR_TX_FD) { *out_speed = GENET_UMAC_CMD_SPEED_100; *out_fdx = 1; }
        else if (common & ANAR_TX) { *out_speed = GENET_UMAC_CMD_SPEED_100; *out_fdx = 0; }
        else if (common & ANAR_10_FD) { *out_speed = GENET_UMAC_CMD_SPEED_10; *out_fdx = 1; }
        else { *out_speed = GENET_UMAC_CMD_SPEED_10; *out_fdx = 0; }
    }
    return 1;
}

/* ---- リセット・リング初期化 (bcmgenet.c の genet_reset/genet_init_rings を
 * MAC アドレス設定込みで固定サイズ版に移した形) -------------------------- */

static void genet_disable_dma(void) {
    uint32_t val;
    val = mmio_r32(GENET_UMAC_CMD); val &= ~GENET_UMAC_CMD_RXEN; mmio_w32(GENET_UMAC_CMD, val);
    val = mmio_r32(GENET_RX_DMA_CTRL);
    val &= ~(GENET_RX_DMA_CTRL_EN | GENET_RX_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE));
    mmio_w32(GENET_RX_DMA_CTRL, val);
    val = mmio_r32(GENET_TX_DMA_CTRL);
    val &= ~(GENET_TX_DMA_CTRL_EN | GENET_TX_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE));
    mmio_w32(GENET_TX_DMA_CTRL, val);
    mmio_w32(GENET_UMAC_TX_FLUSH, 1);
    mmio_w32(GENET_UMAC_TX_FLUSH, 0);
    val = mmio_r32(GENET_UMAC_CMD); val &= ~GENET_UMAC_CMD_TXEN; mmio_w32(GENET_UMAC_CMD, val);
}

static void genet_mac_reset(void) {
    uint32_t val;

    genet_disable_dma();

    val = mmio_r32(GENET_SYS_RBUF_FLUSH_CTRL);
    mmio_w32(GENET_SYS_RBUF_FLUSH_CTRL, val | GENET_SYS_RBUF_FLUSH_RESET);
    mmio_w32(GENET_SYS_RBUF_FLUSH_CTRL, val & ~GENET_SYS_RBUF_FLUSH_RESET);
    mmio_w32(GENET_SYS_RBUF_FLUSH_CTRL, 0);

    mmio_w32(GENET_UMAC_CMD, 0);
    mmio_w32(GENET_UMAC_CMD, GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    mmio_w32(GENET_UMAC_CMD, 0);

    mmio_w32(GENET_UMAC_MIB_CTRL,
             GENET_UMAC_MIB_RESET_RUNT | GENET_UMAC_MIB_RESET_RX | GENET_UMAC_MIB_RESET_TX);
    mmio_w32(GENET_UMAC_MIB_CTRL, 0);

    mmio_w32(GENET_UMAC_MAX_FRAME_LEN, 1536);

    val = mmio_r32(GENET_RBUF_CTRL);
    mmio_w32(GENET_RBUF_CTRL, val | GENET_RBUF_ALIGN_2B);
    mmio_w32(GENET_RBUF_TBUF_SIZE_CTRL, 1);
}

static void genet_init_rx_ring(void) {
    const uint32_t q = GENET_DMA_DEFAULT_QUEUE;
    uint32_t i;

    g_rx_next = 0;
    g_rx_pidx = GENET_RX_RING_COUNT;   /* bcmgenet.c と同じ初期値 */

    mmio_w32(GENET_RX_SCB_BURST_SIZE, 0x08);

    for (i = 0; i < GENET_RX_RING_COUNT; i++) {
        uint64_t dma_pa = genet_dma_addr(g_rx_buf_phys[i]);
        mmio_w32(GENET_RX_DESC_ADDRESS_LO(i), (uint32_t)dma_pa);
        mmio_w32(GENET_RX_DESC_ADDRESS_HI(i), (uint32_t)(dma_pa >> 32));
    }

    mmio_w32(GENET_RX_DMA_WRITE_PTR_LO(q), 0);
    mmio_w32(GENET_RX_DMA_WRITE_PTR_HI(q), 0);
    mmio_w32(GENET_RX_DMA_PROD_INDEX(q), g_rx_pidx);
    /* **bcmgenet.c を読み直して見つけた不具合 (2026-09-04)。**
     * genet_init_rings は CONS_INDEX に一旦 0 を書くが、直後の
     * genet_fill_rx_ring が全記述子を埋め終えた時点で
     * `WR4(CONS_INDEX, cidx)` (cidx はここでは RING_COUNT) を**もう一度
     * 書き直している**。CONS_INDEX が 0 のままだと「ハードウェアに
     * まだ 1 枚も渡していない」ことになり、RX が永久に動かない
     * (実機で実測: RX_DMA_PROD_INDEX が一度も進まなかった)。
     * こちらは記述子を最初から全部埋めているので、最初から
     * RING_COUNT を書けば同じ状態になる */
    mmio_w32(GENET_RX_DMA_CONS_INDEX(q), g_rx_pidx);
    mmio_w32(GENET_RX_DMA_RING_BUF_SIZE(q),
             (GENET_RX_RING_COUNT << 16) | (GENET_RX_BUF_SIZE & 0xffffU));
    mmio_w32(GENET_RX_DMA_START_ADDR_LO(q), 0);
    mmio_w32(GENET_RX_DMA_START_ADDR_HI(q), 0);
    mmio_w32(GENET_RX_DMA_END_ADDR_LO(q), GENET_RX_RING_COUNT * GENET_DMA_DESC_SIZE / 4U - 1U);
    mmio_w32(GENET_RX_DMA_END_ADDR_HI(q), 0);
    mmio_w32(GENET_RX_DMA_XON_XOFF_THRES(q), (5U << 16) | (GENET_RX_RING_COUNT >> 4));
    mmio_w32(GENET_RX_DMA_READ_PTR_LO(q), 0);
    mmio_w32(GENET_RX_DMA_READ_PTR_HI(q), 0);

    mmio_w32(GENET_RX_DMA_RING_CFG, 1U << q);

    mmio_w32(GENET_RX_DMA_CTRL,
             mmio_r32(GENET_RX_DMA_CTRL) | GENET_RX_DMA_CTRL_EN | GENET_RX_DMA_CTRL_RBUF_EN(q));
}

static void genet_init_tx_ring(void) {
    const uint32_t q = GENET_DMA_DEFAULT_QUEUE;

    g_tx_pidx = 0;
    g_tx_cidx = 0;

    mmio_w32(GENET_TX_SCB_BURST_SIZE, 0x08);
    mmio_w32(GENET_TX_DMA_READ_PTR_LO(q), 0);
    mmio_w32(GENET_TX_DMA_READ_PTR_HI(q), 0);
    mmio_w32(GENET_TX_DMA_CONS_INDEX(q), 0);
    mmio_w32(GENET_TX_DMA_PROD_INDEX(q), 0);
    mmio_w32(GENET_TX_DMA_RING_BUF_SIZE(q),
             (GENET_TX_RING_COUNT << 16) | (GENET_RX_BUF_SIZE & 0xffffU));
    mmio_w32(GENET_TX_DMA_START_ADDR_LO(q), 0);
    mmio_w32(GENET_TX_DMA_START_ADDR_HI(q), 0);
    mmio_w32(GENET_TX_DMA_END_ADDR_LO(q), GENET_TX_RING_COUNT * GENET_DMA_DESC_SIZE / 4U - 1U);
    mmio_w32(GENET_TX_DMA_END_ADDR_HI(q), 0);
    mmio_w32(GENET_TX_DMA_MBUF_DONE_THRES(q), 1);
    mmio_w32(GENET_TX_DMA_FLOW_PERIOD(q), 0);
    mmio_w32(GENET_TX_DMA_WRITE_PTR_LO(q), 0);
    mmio_w32(GENET_TX_DMA_WRITE_PTR_HI(q), 0);

    mmio_w32(GENET_TX_DMA_RING_CFG, 1U << q);

    mmio_w32(GENET_TX_DMA_CTRL,
             mmio_r32(GENET_TX_DMA_CTRL) | GENET_TX_DMA_CTRL_EN | GENET_TX_DMA_CTRL_RBUF_EN(q));
}

static void genet_update_link(uint32_t speed, int fdx) {
    uint32_t val;

    val = mmio_r32(GENET_EXT_RGMII_OOB_CTRL);
    val &= ~GENET_EXT_RGMII_OOB_OOB_DISABLE;
    val |= GENET_EXT_RGMII_OOB_RGMII_LINK | GENET_EXT_RGMII_OOB_RGMII_MODE_EN;
    if (g_phy_mode == 0) val |= GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;  /* 素の rgmii だけ */
    else val &= ~GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;
    mmio_w32(GENET_EXT_RGMII_OOB_CTRL, val);

    val = mmio_r32(GENET_UMAC_CMD);
    val &= ~GENET_UMAC_CMD_SPEED_MASK;
    val |= (speed << GENET_UMAC_CMD_SPEED_SHIFT) & GENET_UMAC_CMD_SPEED_MASK;
    if (fdx) val &= ~0u; /* GENETv5 の duplex は UMAC_CMD に専用ビットが無く、
                            RGMII 側で解決される想定 (bcmgenet.c にも
                            duplex 専用の書き込みは無い) */
    mmio_w32(GENET_UMAC_CMD, val);
}

/* ---- 公開 API (include/virtio_net.h と同じ形。dispatcher から呼ぶ) ------- */

int aarch64_genet_probe(void) {
    return aarch64_boot_info()->genet_base != 0;
}

int aarch64_genet_init(void) {
    const aarch64_boot_info_t* b = aarch64_boot_info();
    uint32_t speed;
    int fdx, linked;
    uint32_t i;

    if (g_ready) return 0;
    if (b->genet_base == 0) return -1;

    spinlock_init(&g_genet_lock);
    g_base_pa = b->genet_base;
    g_base = (volatile uint8_t*)(uintptr_t)aarch64_phys_to_virt(b->genet_base);
    g_dma_offset = b->genet_dma_offset;
    g_phy_mode = b->genet_phy_mode;
    /* PHY アドレスは実測 (Pi 4 の DTB: /scb/ethernet@.../mdio@e14/ethernet-phy@1) で
     * 1 だが、直書きにせず DTB に mdio 子ノードを持たない機械のための
     * 既定値として使う (子ノードの解析は今回のスコープ外) */
    g_phy_addr = 1;

    for (i = 0; i < 6; i++) g_mac[i] = b->genet_mac[i];

    genet_mac_reset();

    /* ---- DMA バッファは非キャッシュ枠から取る --------------------------
     *
     * **pmm (HHDM の Normal-WB) から取ってはいけない。** GENET の DMA は
     * キャッシュコヒーレントではないので、キャッシュの効くページを渡すと
     *   - RX: DMA がメモリへ書いても CPU はキャッシュの古い値を読む
     *   - TX: CPU が書いた中身はキャッシュに留まり、DMA は古い値を読む
     * になる。**実機で実測した (2026-09-04)。** 記述子はレジスタ空間なので
     * 長さだけは正しく取れ、中身だけが 0 のままという形で出た:
     *
     *   [genet-dbg] rx len=90  head=00 00 00 00 00 00 00 00 ...
     *   [genet-dbg] rx len=141 head=00 00 00 00 00 00 00 00 ...
     *
     * 長さは実在のフレームのものなのに中身が 8 枚とも全部 0 だった。
     * DHCP の OFFER が返らないように見えていたのはこれで、実際には
     * 返ってきていたものを lwIP が 0 埋めのゴミとして捨てていた。
     *
     * emmc2 / pcie_brcm (USB) / sound と同じ非キャッシュ枠
     * (Normal-NC、aarch64_vm_dma_alloc) を使う。**取れなければ黙って
     * pmm へ退かない** —— 退くと上の症状に戻るだけで、しかも原因が
     * 見えなくなる (sound.c と同じ扱い) */
    for (i = 0; i < GENET_RX_RING_COUNT; i++) {
        uint64_t pa = aarch64_vm_dma_alloc(1);
        if (!pa) {
            put("[net] genet: could not get RX buffers from the non-cached DMA pool\r\n");
            return -1;
        }
        g_rx_buf_phys[i] = pa;
        g_rx_bufs[i] = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(pa);
        genet_memset(g_rx_bufs[i], 0, GENET_RX_BUF_SIZE);
    }
    for (i = 0; i < GENET_TX_RING_COUNT; i++) {
        uint64_t pa = aarch64_vm_dma_alloc(1);
        if (!pa) {
            put("[net] genet: could not get TX buffers from the non-cached DMA pool\r\n");
            return -1;
        }
        g_tx_buf_phys[i] = pa;
        g_tx_bufs[i] = (uint8_t*)(uintptr_t)aarch64_phys_to_virt(pa);
    }

    /* **MAC アドレスが DTB に無ければ、UMAC のレジスタに残っている値を読む**
     * (ファームウェアが OTP から焼いた値がまだ入っているはず)。
     * 全 0 のときだけレジスタから読み直す */
    {
        int all_zero = 1;
        for (i = 0; i < 6; i++) if (g_mac[i]) { all_zero = 0; break; }
        if (all_zero) {
            uint32_t maclo, machi;
            maclo = mmio_r32(GENET_UMAC_MAC0);
            machi = mmio_r32(GENET_UMAC_MAC1);
            g_mac[0] = (uint8_t)(maclo >> 24); g_mac[1] = (uint8_t)(maclo >> 16);
            g_mac[2] = (uint8_t)(maclo >> 8);  g_mac[3] = (uint8_t)(maclo >> 0);
            g_mac[4] = (uint8_t)(machi >> 8);  g_mac[5] = (uint8_t)(machi >> 0);
        }
    }

    if (g_phy_mode <= 3U) {
        mmio_w32(GENET_SYS_PORT_CTRL, GENET_SYS_PORT_MODE_EXT_GPHY);
    }

    /* MAC アドレスを書く (bcmgenet.c と同じバイト順) */
    mmio_w32(GENET_UMAC_MAC0,
             ((uint32_t)g_mac[0] << 24) | ((uint32_t)g_mac[1] << 16) |
             ((uint32_t)g_mac[2] << 8)  | (uint32_t)g_mac[3]);
    mmio_w32(GENET_UMAC_MAC1, ((uint32_t)g_mac[4] << 8) | (uint32_t)g_mac[5]);

    /* **RX フィルタ (MDF) は使わず、プロミスキャスにする。**
     * 2026-09-04 実機で MDF 有効のまま RX_DMA_PROD_INDEX が一度も動かず
     * (broadcast の DHCP OFFER すら 1 個も来ない)、MDF_CTRL のビットの
     * 向き (有効/無効どちらが 1 か) を bcmgenet.c から確証を持って
     * 読み切れなかった。**切り分けのため一旦プロミスキャスに倒す**
     * (bcmgenet.c の genet_setup_rxfilter が promisc 時にしているのと
     * 同じ形: mdf_ctrl=0)。MDF の向きを確証できたら絞る (Tier 3 級の
     * 後回しでよい — 過大な受信でも動作の正しさには影響しない) */
    mmio_w32(GENET_UMAC_CMD, mmio_r32(GENET_UMAC_CMD) | GENET_UMAC_CMD_PROMISC);
    mmio_w32(GENET_UMAC_MDF_CTRL, 0);

    genet_init_rx_ring();
    genet_init_tx_ring();

    mmio_w32(GENET_UMAC_CMD, mmio_r32(GENET_UMAC_CMD) | GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN);

    linked = genet_phy_autoneg(&speed, &fdx);
    genet_update_link(speed, fdx);

    put("[net] genet ready base=0x");
    puthex_bare(g_base_pa, 8);
    put(" phy_mode=");
    putdec(g_phy_mode);
    put(" link=");
    put(linked ? "up" : "down");
    put(" speed=");
    putdec(speed == GENET_UMAC_CMD_SPEED_1000 ? 1000 : (speed == GENET_UMAC_CMD_SPEED_100 ? 100 : 10));
    put(fdx ? "-full" : "-half");
    put(" mac=");
    for (i = 0; i < 6; i++) {
        puthex_bare(g_mac[i], 2);
        if (i != 5) put(":");
    }
    put("\r\n");

    /* ---- 割り込みを開ける (N-11, 2026-09-04) --------------------------
     *
     * **実測の根拠がある。** ポーリングはタイマ (100Hz) からしか呼ばれず、
     * RX リングは 32 枚。実機で 60 秒測ったところ poll 1 回あたり最大 14 枚
     * 届いており (maxbatch=14)、**リングの 44% を使っていた**。
     * 取りこぼしは 0 だったが余裕は 2.3 倍しかなく、本格的な転送では
     * 溢れる。割り込みで拾えば 10ms を待たずに空けられる。
     *
     * **開けても poll は残す。**割り込みが来ない機械でも動くように */
    if (b->genet_intid) {
        g_intid = b->genet_intid;
        /* まず全部塞いでから、要るものだけ開ける。**MASK は 1 が塞ぐ側**
         * (SET で塞ぎ、CLEAR で開ける) */
        mmio_w32(GENET_INTRL2_CPU_MASK_SET, 0xffffffffU);
        mmio_w32(GENET_INTRL2_CPU_CLEAR, 0xffffffffU);
        mmio_w32(GENET_INTRL2_CPU_MASK_CLEAR, GENET_IRQ_RXDMA_DONE | GENET_IRQ_TXDMA_DONE);
        aarch64_gic_enable_irq(g_intid);
    }

    g_ready = 1;
    return 0;
}

static void genet_reclaim_tx(void) {
    /* **bcmgenet.c と同じ形。**hw の完了確認ではなく、自分で書いた
     * PROD_INDEX を読み直して「積んだ分はもう空けてよい」とみなす —
     * 元のドライバもハードウェアの完了ビットは見ていない。TX_RING_COUNT
     * 本あるので、直近 N 送信ぶんは実際にまだ転送中でも上書きされない */
    const uint32_t q = GENET_DMA_DEFAULT_QUEUE;
    uint32_t prod = mmio_r32(GENET_TX_DMA_PROD_INDEX(q)) & 0xffffU;
    g_tx_cidx = prod;
}

int aarch64_genet_send(const void* frame, uint16_t len) {
    uint32_t index, status;
    uint8_t* dst;
    const uint8_t* src = (const uint8_t*)frame;
    uint64_t dma_pa;

    if (!g_ready || !frame || len == 0 || len > GENET_FRAME_MAX) return -1;

    spin_lock(&g_genet_lock);
    genet_reclaim_tx();
    if ((g_tx_pidx - g_tx_cidx) >= GENET_TX_RING_COUNT) { spin_unlock(&g_genet_lock); return -1; }

    index = g_tx_pidx % GENET_TX_RING_COUNT;
    dst = g_tx_bufs[index];
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    dma_pa = genet_dma_addr(g_tx_buf_phys[index]);
    mmio_w32(GENET_TX_DESC_ADDRESS_LO(index), (uint32_t)dma_pa);
    mmio_w32(GENET_TX_DESC_ADDRESS_HI(index), (uint32_t)(dma_pa >> 32));
    status = GENET_TX_DESC_STATUS_SOP | GENET_TX_DESC_STATUS_EOP |
             GENET_TX_DESC_STATUS_CRC | GENET_TX_DESC_STATUS_QTAG |
             ((uint32_t)len << GENET_TX_DESC_STATUS_BUFLEN_SHIFT);
    mmio_w32(GENET_TX_DESC_STATUS(index), status);

    g_tx_pidx = (g_tx_pidx + 1) & 0xffffU;
    __sync_synchronize();
    mmio_w32(GENET_TX_DMA_PROD_INDEX(GENET_DMA_DEFAULT_QUEUE), g_tx_pidx);
    spin_unlock(&g_genet_lock);
    return 0;
}

void aarch64_genet_poll(void) {
    const uint32_t q = GENET_DMA_DEFAULT_QUEUE;
    uint32_t pidx, total, index, n;
    /* **コールバックはロックの外で呼ぶ。** virtio_net_mmio.c の
     * デッドロック (2026-09-04) と同じ罠がここにもある — DHCP の応答を
     * 受け取った側から同期的に送信し直すので、送信 (aarch64_genet_send)
     * が同じロックで待つと止まる */
    uint32_t ids[GENET_RX_RING_COUNT];
    uint32_t lens[GENET_RX_RING_COUNT];
    uint32_t got = 0;

    if (!g_ready) return;

#ifdef AARCH64_VERBOSE_DIAG
    /* poll 1000 回ごとに 1 行、6 回まで。**タイマだけなら 10 秒ぶん**だが、
     * 割り込みからも呼ばれるぶん少し早く回る */
    {
        static uint32_t printed;
        if (printed < 6U && g_rx_polls != 0U && (g_rx_polls % 1000U) == 0U) {
            printed++;
            put("[genet-rx] polls=");
            putdec(g_rx_polls);
            put(" frames=");
            putdec(g_rx_frames);
            put(" maxbatch=");
            putdec(g_rx_max_batch);
            put(" full=");
            putdec(g_rx_full);
            put(" over=");
            putdec(g_rx_over);
            put(" lost=");
            putdec(g_rx_lost);
            /* 割り込みが実際に来ているか (N-11)。0 のままならポーリング
             * だけで回っている = ビットかレジスタの読みが違う */
            put(" irq=");
            putdec(g_irq_count);
            put(" irqrx=");
            putdec(g_irq_rx);
            put(" irqtx=");
            putdec(g_irq_tx);
            put("\r\n");
        }
    }
#endif

    spin_lock(&g_genet_lock);
    pidx = mmio_r32(GENET_RX_DMA_PROD_INDEX(q)) & 0xffffU;
    total = (pidx - g_rx_pidx) & 0xffffU;
#ifdef AARCH64_VERBOSE_DIAG
    g_rx_polls++;
    if (total > g_rx_max_batch) g_rx_max_batch = total;
    if (total == GENET_RX_RING_COUNT) g_rx_full++;
    if (total > GENET_RX_RING_COUNT) {
        g_rx_over++;
        g_rx_lost += (uint64_t)(total - GENET_RX_RING_COUNT);
    }
#endif
    if (total > GENET_RX_RING_COUNT) total = GENET_RX_RING_COUNT;   /* 壊れた値の保険 */

    index = g_rx_next;
    for (n = 0; n < total; n++) {
        uint32_t status = mmio_r32(GENET_RX_DESC_STATUS(index));
        uint32_t buflen = (status >> GENET_RX_DESC_STATUS_BUFLEN_SHIFT) & 0xfffU;
        ids[got] = index;
        /* **先頭 2 バイトは RBUF_ALIGN_2B の詰め物。**IP ヘッダを 4 バイト
         * 境界に乗せるためハードウェアが入れる (bcmgenet.c の ETHER_ALIGN)。
         * イーサネットフレームはそこから始まる */
        lens[got] = (buflen > 2U && buflen <= GENET_RX_BUF_SIZE) ? (buflen - 2U) : 0U;
        got++;
        index = (index + 1) % GENET_RX_RING_COUNT;
    }
    if (total) {
        g_rx_next = index;
        g_rx_pidx = pidx;
#ifdef AARCH64_VERBOSE_DIAG
        g_rx_frames += total;
#endif
    }
    spin_unlock(&g_genet_lock);

    for (n = 0; n < got; n++) {
        if (g_rx_cb && lens[n] > 0) {
            g_rx_cb(g_rx_bufs[ids[n]] + 2, (uint16_t)lens[n]);
        }
    }

    if (total) {
        spin_lock(&g_genet_lock);
        mmio_w32(GENET_RX_DMA_CONS_INDEX(q), g_rx_pidx);
        spin_unlock(&g_genet_lock);
    }
}

/* GIC から呼ばれる。**発生源を落としてから poll する。**
 * INTRL2 は STAT に立ったビットを CLEAR へ書いて落とす形 */
void aarch64_genet_irq(void) {
    uint32_t stat;
    if (!g_ready) return;

    stat = mmio_r32(GENET_INTRL2_CPU_STAT) & ~mmio_r32(GENET_INTRL2_CPU_MASK_STATUS);
    if (!stat) return;
    mmio_w32(GENET_INTRL2_CPU_CLEAR, stat);

    g_irq_count++;
    if (stat & GENET_IRQ_RXDMA_DONE) g_irq_rx++;
    if (stat & GENET_IRQ_TXDMA_DONE) g_irq_tx++;

    /* **受け取りは poll に任せる。**リングから拾う手順は 1 つにしておく
     * (割り込みとポーリングで別々に書くと必ずずれる) */
    aarch64_genet_poll();
}

uint32_t aarch64_genet_intid(void) { return g_intid; }
uint64_t aarch64_genet_irq_count(void) { return g_irq_count; }

/* **ポーリングはやめない。**割り込みが来ない機械でも動くようにする
 * (来ているかどうかは g_irq_count を見れば分かる)。タイマからの poll は
 * 10ms に 1 回で、割り込みが働いていればその前に拾い終わっている */
int aarch64_genet_needs_poll_fallback(void) { return g_ready; }
int aarch64_genet_is_ready(void) { return g_ready; }
const uint8_t* aarch64_genet_mac(void) { return g_mac; }
void aarch64_genet_set_rx_callback(virtio_net_rx_cb_t cb) { g_rx_cb = cb; }
