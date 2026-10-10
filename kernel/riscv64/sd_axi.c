#include <stddef.h>
#include <stdint.h>
#include "riscv64/boot.h"
#include "riscv64/csr.h"
#include "riscv64/sd_axi.h"

#define SD_STAT 0x000U
#define SD_LBA  0x004U
#define SD_FDIV 0x008U
#define SD_RESP 0x00CU
#define SD_RBUF 0x400U
#define SD_WBUF 0x600U

#define SD_OP_INIT  1U
#define SD_OP_CSD   3U
#define SD_OP_READ  4U
#define SD_OP_WRITE 6U

#define SD_STAT_BUSY    0x01U
#define SD_STAT_INITOK  0x02U
#define SD_STAT_HC      0x08U
#define SD_STAT_PRESENT 0x10U
#define SD_STAT_ERR(s)  (((s) >> 8) & 0xffU)

#define SD_SECTOR 512U

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

int riscv64_sd_axi_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    uint8_t* dst = (uint8_t*)buf;
    (void)ctx;
    for (size_t n = 0; n < count; n++) {
        if (lba + n >= g_sd_sectors) return -1;
        *sd_reg(SD_LBA) = (uint32_t)(lba + n);
        if (sd_run(SD_OP_READ) != 0) return -1;
        for (uint32_t i = 0; i < SD_SECTOR; i += 4) {
            uint32_t w = *sd_reg(SD_RBUF + i);
            dst[i] = (uint8_t)w;
            dst[i + 1] = (uint8_t)(w >> 8);
            dst[i + 2] = (uint8_t)(w >> 16);
            dst[i + 3] = (uint8_t)(w >> 24);
        }
        dst += SD_SECTOR;
    }
    return 0;
}

int riscv64_sd_axi_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    const uint8_t* src = (const uint8_t*)buf;
    (void)ctx;
    for (size_t n = 0; n < count; n++) {
        if (lba + n >= g_sd_sectors) return -1;
        for (uint32_t i = 0; i < SD_SECTOR; i += 4) {
            *sd_reg(SD_WBUF + i) = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) |
                                   ((uint32_t)src[i + 2] << 16) | ((uint32_t)src[i + 3] << 24);
        }
        *sd_reg(SD_LBA) = (uint32_t)(lba + n);
        if (sd_run(SD_OP_WRITE) != 0) return -1;
        src += SD_SECTOR;
    }
    return 0;
}
