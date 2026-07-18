#ifndef ORTHOX_RISCV64_VIRTIO_BLK_MMIO_H
#define ORTHOX_RISCV64_VIRTIO_BLK_MMIO_H

#include <stddef.h>
#include <stdint.h>

int riscv64_virtio_blk_mmio_init(void);
int riscv64_virtio_blk_mmio_present(void);
uint64_t riscv64_virtio_blk_mmio_capacity(void);
int riscv64_virtio_blk_mmio_storage_read(void* ctx, uint64_t lba, void* buf, size_t count);
int riscv64_virtio_blk_mmio_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count);

#endif
