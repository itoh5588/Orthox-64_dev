#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stddef.h>

/* VirtIO PCI Vendor ID */
#define VIRTIO_PCI_VENDOR_ID 0x1AF4

/* VirtIO PCI Device IDs (Legacy) */
#define VIRTIO_PCI_DEVICE_NET  0x1000
#define VIRTIO_PCI_DEVICE_BLK  0x1001

/* Legacy PCI Register Offsets (Virtio 0.9.x) */
#define VIRTIO_PCI_HOST_FEATURES   0x00
#define VIRTIO_PCI_GUEST_FEATURES  0x04
#define VIRTIO_PCI_QUEUE_PFN       0x08
#define VIRTIO_PCI_QUEUE_NUM       0x0C
#define VIRTIO_PCI_QUEUE_SEL       0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY    0x10
#define VIRTIO_PCI_STATUS          0x12
#define VIRTIO_PCI_ISR             0x13
#define VIRTIO_PCI_CONFIG_VECTOR   0x14
#define VIRTIO_PCI_QUEUE_VECTOR    0x16
#define VIRTIO_PCI_CONFIG          0x14   /* device-specific config start */
#define VIRTIO_MSI_NO_VECTOR       0xFFFF

/* VirtIO Status bits */
#define VIRTIO_STATUS_ACKNOWLEDGE  0x01
#define VIRTIO_STATUS_DRIVER       0x02
#define VIRTIO_STATUS_DRIVER_OK    0x04
#define VIRTIO_STATUS_FAILED       0x80

/* vring Descriptor Flags */
#define VRING_DESC_F_NEXT          0x1   /* Chain continuation */
#define VRING_DESC_F_WRITE         0x2   /* Device-to-Guest (Write only) */
#define VRING_DESC_F_INDIRECT      0x4   /* Indirect descriptor (unused for now) */

#define VIRTQ_ALIGN 4096U

/* vring structure definitions */
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

/* Virtqueue Metadata */
struct virtio_queue {
    uint16_t queue_size;
    uint16_t active_descs;
    uint16_t last_used_idx;
    uint64_t ring_phys;
    uint8_t* ring_virt;
    struct vring_desc* desc;
    struct vring_avail* avail;
    struct vring_used* used;
};

/* I/O Port Helper Functions (x86) */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Common VirtIO Functions (to be implemented in kernel/virtio.c) */
int virtio_virtq_init(uint16_t iobase, uint16_t queue_index, struct virtio_queue* q);
void virtio_kick(uint16_t iobase, uint16_t queue_index);
void virtio_set_queue_msix_vector(uint16_t iobase, uint16_t queue_index, uint16_t vector);
void virtio_disable_config_msix_vector(uint16_t iobase);

#endif
