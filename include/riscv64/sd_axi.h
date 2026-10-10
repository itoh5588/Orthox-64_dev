#ifndef ORTHOX_ARCH_RISCV64_SD_AXI_H
#define ORTHOX_ARCH_RISCV64_SD_AXI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Arty S7 MicroBlaze V の SD カード (Pmod MicroSD, SPI モード)。
 * コントローラは fpga/arty_s7_mbv/rtl/sd_axi.vhd。初期化・CRC・SDSC の
 * バイト番地変換はハード側 (sdspi_ctl) がやるので、ここは 1 ブロックずつ
 * 番号を書いて起動し、BUSY が落ちるのを待つだけ。
 */
int riscv64_sd_axi_init(uint64_t base);
uint64_t riscv64_sd_axi_sectors(void);
int riscv64_sd_axi_storage_read(void* ctx, uint64_t lba, void* buf, size_t count);
int riscv64_sd_axi_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count);

#endif
