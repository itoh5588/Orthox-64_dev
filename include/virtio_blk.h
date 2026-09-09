#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

/* VirtIO Block Request Types */
#define VIRTIO_BLK_T_IN           0
#define VIRTIO_BLK_T_OUT          1
#define VIRTIO_BLK_T_FLUSH        4

/* VirtIO Block Status */
#define VIRTIO_BLK_S_OK           0
#define VIRTIO_BLK_S_IOERR        1
#define VIRTIO_BLK_S_UNSUPP       2

/* VirtIO Block Request Header */
struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* VirtIO Block Config Space */
struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;
    struct {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;
    uint32_t blk_size;
} __attribute__((packed));

/* Function Prototypes */
int virtio_blk_init(void);
int virtio_blk_read(uint64_t sector, void* buf, uint32_t count);
int virtio_blk_write(uint64_t sector, const void* buf, uint32_t count);
uint64_t virtio_blk_capacity(void);

/* Second drive: write-only output device ("kout") */
int virtio_kout_init(void);
int virtio_kout_write_raw(uint64_t byte_offset, const void* buf, size_t count);

#endif
