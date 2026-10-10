#ifndef ORTHOX_ARCH_RISCV64_RAMDISK_H
#define ORTHOX_ARCH_RISCV64_RAMDISK_H

#include <stddef.h>
#include <stdint.h>

/*
 * DTB の /chosen linux,initrd-start/end が指す物理メモリ上のディスク
 * イメージを 512 バイトセクタのブロックデバイスとして見せる。
 * 記憶装置の無い基板 (Arty S7 MicroBlaze V) で rootfs を JTAG ロードして使う。
 */
int riscv64_ramdisk_init(uint64_t base, uint64_t size);
uint64_t riscv64_ramdisk_sectors(void);
int riscv64_ramdisk_storage_read(void* ctx, uint64_t lba, void* buf, size_t count);
int riscv64_ramdisk_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count);

#endif
