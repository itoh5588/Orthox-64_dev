#include <stdint.h>
#include <stddef.h>
#include "kassert.h"
#include "virtio.h"
#include "pmm.h"
#include "vmm.h"

static void* kernel_memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static uint32_t virtq_bytes(uint16_t queue_size) {
    KASSERT(queue_size > 0);
    uint32_t avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + queue_size));
    uint32_t used_off = (uint32_t)((sizeof(struct vring_desc) * queue_size + avail_bytes + (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    uint32_t used_bytes = (uint32_t)(sizeof(uint16_t) * 3U + sizeof(struct vring_used_elem) * queue_size);
    return used_off + used_bytes;
}

static void virtq_layout(struct virtio_queue* q) {
    KASSERT(q != 0);
    KASSERT(q->queue_size > 0);
    KASSERT(q->ring_virt != 0);
    uint32_t avail_bytes = (uint32_t)(sizeof(uint16_t) * (3U + q->queue_size));
    uint32_t used_off = (uint32_t)((sizeof(struct vring_desc) * q->queue_size + avail_bytes + (VIRTQ_ALIGN - 1)) & ~(VIRTQ_ALIGN - 1));
    q->desc = (struct vring_desc*)q->ring_virt;
    q->avail = (struct vring_avail*)(q->ring_virt + sizeof(struct vring_desc) * q->queue_size);
    q->used = (struct vring_used*)(q->ring_virt + used_off);
}

int virtio_virtq_init(uint16_t iobase, uint16_t queue_index, struct virtio_queue* q) {
    if (!q) return -1;

    outw((uint16_t)(iobase + VIRTIO_PCI_QUEUE_SEL), queue_index);
    q->queue_size = inw((uint16_t)(iobase + VIRTIO_PCI_QUEUE_NUM));
    if (q->queue_size == 0) return -1;

    uint32_t bytes = virtq_bytes(q->queue_size);
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    void* ring_phys = pmm_alloc(pages);
    if (!ring_phys) return -1;

    q->ring_phys = (uint64_t)ring_phys;
    KASSERT((q->ring_phys & (PAGE_SIZE - 1)) == 0);
    q->ring_virt = (uint8_t*)PHYS_TO_VIRT(ring_phys);
    q->active_descs = 0;
    q->last_used_idx = 0;
    kernel_memset(q->ring_virt, 0, pages * PAGE_SIZE);
    virtq_layout(q);
    KASSERT(q->desc != 0);
    KASSERT(q->avail != 0);
    KASSERT(q->used != 0);

    outl((uint16_t)(iobase + VIRTIO_PCI_QUEUE_PFN), (uint32_t)(q->ring_phys / PAGE_SIZE));
    return 0;
}

void virtio_kick(uint16_t iobase, uint16_t queue_index) {
    __sync_synchronize();
    outw((uint16_t)(iobase + VIRTIO_PCI_QUEUE_NOTIFY), queue_index);
}

void virtio_set_queue_msix_vector(uint16_t iobase, uint16_t queue_index, uint16_t vector) {
    outw((uint16_t)(iobase + VIRTIO_PCI_QUEUE_SEL), queue_index);
    outw((uint16_t)(iobase + VIRTIO_PCI_QUEUE_VECTOR), vector);
}

void virtio_disable_config_msix_vector(uint16_t iobase) {
    outw((uint16_t)(iobase + VIRTIO_PCI_CONFIG_VECTOR), VIRTIO_MSI_NO_VECTOR);
}
