#include <stddef.h>
#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/sd_axi.h"

#define SD_STAT 0x000U
#define SD_LBA  0x004U
#define SD_FDIV 0x008U
#define SD_RESP 0x00CU
#define SD_NBLK 0x010U
#define SD_RBUF 0x400U
#define SD_MBUF 0x1000U   /* 8 ブロックの読み出しバッファ (block n は +512n) */
#define SD_MWBUF 0x2000U  /* 8 ブロックの書き込みバッファ (block n は +512n) */
#define SD_WBUF 0x600U

#define SD_OP_INIT  1U
#define SD_OP_CSD   3U
#define SD_OP_READ  4U
#define SD_OP_MREAD 5U    /* CMD18: SD_NBLK ブロック (最大 SD_MREAD_MAX) */
#define SD_OP_WRITE 6U
#define SD_OP_MWRITE 7U   /* CMD25: SD_NBLK ブロック (最大 SD_MREAD_MAX) */

#define SD_STAT_BUSY    0x01U
#define SD_STAT_INITOK  0x02U
#define SD_STAT_HC      0x08U
#define SD_STAT_PRESENT 0x10U
#define SD_STAT_ERR(s)  (((s) >> 8) & 0xffU)

#define SD_SECTOR 512U

/* **まとめて読むブロック数の上限 (2026-10-11)。** CMD17 は 1 ブロックごとに
 * カードの応答待ち (実測で 0.5 ms 前後) が掛かる。CMD18 なら 1 コマンドに 1 回。
 * 8 はコントローラ (fpga/arty_s7_mbv/rtl/sd_axi.vhd) のバッファの大きさ。
 * 古い bitstream (op 5 も 0x1000 の窓も無い) では 1 にすること */
#define SD_MREAD_MAX 8U

/* 初期化後の SPI の分周 (1 相の長さ - 1)。50 MHz で 1 なら 12.5 MHz。
 * sdspi_phy は MISO を 2 段の同期 FF に通して High の終わりで取るので、実際に
 * 見るのは立ち上がり付近のピン = カードが値を変えた立ち下がりの 40 ns 後で、
 * 出力遅延 (最大 14 ns) に間に合う。0 (25 MHz) は変わり目に当たるので使えない。
 * リセット時の既定は 2 (8.3 MHz)。2026-10-11 に 1 にして実機で照合した */
#define SD_FASTDIV 1U

static uint64_t g_sd_base;
static uint64_t g_sd_sectors;

static inline volatile uint32_t* sd_reg(uint32_t off) {
    return (volatile uint32_t*)(uintptr_t)(g_sd_base + off);
}

/* 1 操作を起動して終わるのを待つ。戻り値は sdspi_ctl のエラーコード (0 = 成功)。
 * 初期化はカードによって 1 秒近くかかるので、待ちは 2 秒で打ち切る */
static uint32_t sd_run(uint32_t op) {
    uint64_t deadline = riscv64_read_time() + 2ULL * riscv64_timebase_hz();
    uint32_t stat;
    *sd_reg(SD_STAT) = op;
    do {
        stat = *sd_reg(SD_STAT);
        if ((stat & SD_STAT_BUSY) == 0) return SD_STAT_ERR(stat);
    } while (riscv64_read_time() < deadline);
    return 0x100U;   /* timeout */
}

static uint8_t sd_rbuf_byte(uint32_t i) {
    uint32_t w = *sd_reg(SD_RBUF + (i & ~3U));
    return (uint8_t)(w >> (8U * (i & 3U)));
}

/* CSD (16 バイト、先頭が bit 127) から 512 バイトセクタ数を出す */
static uint64_t sd_csd_sectors(void) {
    uint8_t c[16];
    for (uint32_t i = 0; i < 16; i++) c[i] = sd_rbuf_byte(i);
    if ((c[0] >> 6) == 1) {                   /* CSD v2: SDHC/SDXC */
        uint64_t c_size = ((uint64_t)(c[7] & 0x3fU) << 16) | ((uint64_t)c[8] << 8) | c[9];
        return (c_size + 1ULL) * 1024ULL;     /* (C_SIZE+1) * 512 KiB */
    }
    {                                         /* CSD v1: SDSC */
        uint32_t read_bl_len = c[5] & 0x0fU;
        uint32_t c_size = ((uint32_t)(c[6] & 0x03U) << 10) | ((uint32_t)c[7] << 2) | (c[8] >> 6);
        uint32_t mult = ((uint32_t)(c[9] & 0x03U) << 1) | (c[10] >> 7);
        uint64_t bytes = ((uint64_t)c_size + 1ULL) << (mult + 2U + read_bl_len);
        return bytes / SD_SECTOR;
    }
}

int riscv64_sd_axi_init(uint64_t base) {
    uint32_t stat;
    uint32_t err;
    g_sd_base = base;
    g_sd_sectors = 0;
    stat = *sd_reg(SD_STAT);
    if ((stat & SD_STAT_PRESENT) == 0) {
        riscv64_uart_puts("  sd: no card\n");
        return -1;
    }
    err = sd_run(SD_OP_INIT);
    stat = *sd_reg(SD_STAT);
    if (err != 0 || (stat & SD_STAT_INITOK) == 0) {
        riscv64_uart_puts("  sd: init failed, stat 0x");
        riscv64_uart_puthex64(stat);
        riscv64_uart_puts(" err 0x");
        riscv64_uart_puthex64(err);
        riscv64_uart_puts("\n");
        return -1;
    }
    *sd_reg(SD_FDIV) = SD_FASTDIV;
    err = sd_run(SD_OP_CSD);
    if (err != 0) {
        riscv64_uart_puts("  sd: CSD read failed, err 0x");
        riscv64_uart_puthex64(err);
        riscv64_uart_puts("\n");
        return -1;
    }
    g_sd_sectors = sd_csd_sectors();
    riscv64_uart_puts("  sd: ");
    riscv64_uart_puts((stat & SD_STAT_HC) ? "SDHC/SDXC" : "SDSC");
    riscv64_uart_puts(", OCR 0x");
    riscv64_uart_puthex64(*sd_reg(SD_RESP));
    riscv64_uart_puts(", 0x");
    riscv64_uart_puthex64(g_sd_sectors);
    riscv64_uart_puts(" sectors\n");
    return g_sd_sectors ? 0 : -1;
}

uint64_t riscv64_sd_axi_sectors(void) {
    return g_sd_sectors;
}

#ifdef RISCV64_PC_SAMPLE
uint64_t g_riscv64_sd_sectors_read;  /* PC 標本と一緒に出す (trap.c) */
#endif

int riscv64_sd_axi_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    uint8_t* dst = (uint8_t*)buf;
    (void)ctx;
#ifdef RISCV64_PC_SAMPLE
    g_riscv64_sd_sectors_read += count;
#endif
    if (lba + count > g_sd_sectors || lba + count < lba) return -1;
    while (count > 0) {
        size_t nb = count < SD_MREAD_MAX ? count : SD_MREAD_MAX;
        uint32_t words = (uint32_t)(nb * SD_SECTOR / 4U);
        *sd_reg(SD_LBA) = (uint32_t)lba;
        if (nb == 1) {
            if (sd_run(SD_OP_READ) != 0) return -1;
        } else {
            *sd_reg(SD_NBLK) = (uint32_t)nb;
            if (sd_run(SD_OP_MREAD) != 0) return -1;
        }
        /* **揃っていれば語のまま書く (2026-10-11)。** D キャッシュはライトスルー
         * で、1 回の書き込みごとに DDR まで行く。バイトに分けると 512 回、
         * 語なら 128 回 (送り先は xv6fs のブロックやページなので普通は揃う) */
        if (((uintptr_t)dst & 3U) == 0) {
            uint32_t* w = (uint32_t*)dst;
            for (uint32_t i = 0; i < words; i++) w[i] = *sd_reg(SD_MBUF + 4U * i);
        } else {
            for (uint32_t i = 0; i < words * 4U; i += 4) {
                uint32_t w = *sd_reg(SD_MBUF + i);
                dst[i] = (uint8_t)w;
                dst[i + 1] = (uint8_t)(w >> 8);
                dst[i + 2] = (uint8_t)(w >> 16);
                dst[i + 3] = (uint8_t)(w >> 24);
            }
        }
        dst += nb * SD_SECTOR;
        lba += nb;
        count -= nb;
    }
    return 0;
}

/* 1 ブロック (CMD24) ずつ書く。CMD25 が失敗したときの退き先 */
static int sd_write_single(uint64_t lba, const uint8_t* src, size_t count) {
    for (size_t n = 0; n < count; n++) {
        if (((uintptr_t)src & 3U) == 0) {
            const uint32_t* w = (const uint32_t*)src;
            for (uint32_t i = 0; i < SD_SECTOR / 4U; i++) *sd_reg(SD_WBUF + 4U * i) = w[i];
        } else {
            for (uint32_t i = 0; i < SD_SECTOR; i += 4) {
                *sd_reg(SD_WBUF + i) = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) |
                                       ((uint32_t)src[i + 2] << 16) | ((uint32_t)src[i + 3] << 24);
            }
        }
        *sd_reg(SD_LBA) = (uint32_t)(lba + n);
        if (sd_run(SD_OP_WRITE) != 0) return -1;
        src += SD_SECTOR;
    }
    return 0;
}

/* **8 ブロックまで CMD25 でまとめて書く (2026-10-11)。** CMD24 は 1 ブロック
 * ごとにカードの書き込み待ちが掛かり、Arty S7 で 0.07 MB/s しか出なかった。
 * xv6fs のログは連続区間をまとめて渡してくる (kernel/xv6log.c)。
 * CMD25 が途中で失敗するとカードが受信の途中に残りうるので、初期化し直して
 * そのまとまりだけ 1 ブロックずつ書き直す */
int riscv64_sd_axi_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    const uint8_t* src = (const uint8_t*)buf;
    (void)ctx;
    if (lba + count > g_sd_sectors || lba + count < lba) return -1;
    while (count > 0) {
        size_t nb = count < SD_MREAD_MAX ? count : SD_MREAD_MAX;
        if (nb == 1) {
            if (sd_write_single(lba, src, 1) != 0) return -1;
        } else {
            uint32_t words = (uint32_t)(nb * SD_SECTOR / 4U);
            uint32_t err;
            if (((uintptr_t)src & 3U) == 0) {
                const uint32_t* w = (const uint32_t*)src;
                for (uint32_t i = 0; i < words; i++) *sd_reg(SD_MWBUF + 4U * i) = w[i];
            } else {
                for (uint32_t i = 0; i < words * 4U; i += 4) {
                    *sd_reg(SD_MWBUF + i) = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) |
                                            ((uint32_t)src[i + 2] << 16) |
                                            ((uint32_t)src[i + 3] << 24);
                }
            }
            *sd_reg(SD_LBA) = (uint32_t)lba;
            *sd_reg(SD_NBLK) = (uint32_t)nb;
            err = sd_run(SD_OP_MWRITE);
            if (err != 0) {
                riscv64_uart_puts("  sd: CMD25 failed, err 0x");
                riscv64_uart_puthex64(err);
                riscv64_uart_puts(", retrying block by block\n");
                if (sd_run(SD_OP_INIT) != 0) return -1;
                *sd_reg(SD_FDIV) = SD_FASTDIV;
                if (sd_write_single(lba, src, nb) != 0) return -1;
            }
        }
        src += nb * SD_SECTOR;
        lba += nb;
        count -= nb;
    }
    return 0;
}
