#include <stddef.h>
#include <stdint.h>
#include "riscv64/ramdisk.h"
#include "string.h"

#define RAMDISK_SECTOR_SIZE 512ULL

static uint64_t g_ramdisk_base;
static uint64_t g_ramdisk_size;

int riscv64_ramdisk_init(uint64_t base, uint64_t size) {
    if (base == 0 || size < RAMDISK_SECTOR_SIZE) return -1;
    g_ramdisk_base = base;
    g_ramdisk_size = size & ~(RAMDISK_SECTOR_SIZE - 1ULL);
    return 0;
}

uint64_t riscv64_ramdisk_sectors(void) {
    return g_ramdisk_size / RAMDISK_SECTOR_SIZE;
}

/* カーネルは物理メモリを恒等マップしているので、物理番地をそのまま触れる */
static void* ramdisk_ptr(uint64_t lba, size_t count) {
    uint64_t off = lba * RAMDISK_SECTOR_SIZE;
    uint64_t len = (uint64_t)count * RAMDISK_SECTOR_SIZE;
    if (g_ramdisk_size == 0 || off > g_ramdisk_size || len > g_ramdisk_size - off) return 0;
    return (void*)(uintptr_t)(g_ramdisk_base + off);
}

int riscv64_ramdisk_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    void* src = ramdisk_ptr(lba, count);
    (void)ctx;
    if (!src) return -1;
    memcpy(buf, src, count * RAMDISK_SECTOR_SIZE);
    return 0;
}

int riscv64_ramdisk_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    void* dst = ramdisk_ptr(lba, count);
    (void)ctx;
    if (!dst) return -1;
    memcpy(dst, buf, count * RAMDISK_SECTOR_SIZE);
    return 0;
}
