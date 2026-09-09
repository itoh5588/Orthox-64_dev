#include <stdint.h>
#include <stddef.h>
#include "kassert.h"
#include "virtio_blk.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "wait.h"
#include "pic.h"
#include "task.h"
#include "bottom_half.h"
#include "irq.h"

void puts(const char* s);
void puthex(uint64_t v);

#define VIRTIO_BLK_QUEUE 0
#define VIRTIO_BLK_TIMEOUT_MS 5000ULL
#define VIRTIO_BLK_MAX_REQUESTS 8
#define VIRTIO_BLK_DESCS_PER_REQ 3

static struct virtio_queue g_vblk_q;
static uint16_t g_vblk_iobase = 0;
static int g_vblk_ready = 0;
static int g_vblk_irq_line = -1;
static int g_vblk_irq_vector = -1;
static int g_vblk_irq_enabled = 0;
static int g_vblk_msi_enabled = 0;
static int g_vblk_msix_enabled = 0;
static uint64_t g_vblk_capacity = 0;
static struct wait_queue g_vblk_idle_wait;
static int g_vblk_request_count = 0;

struct vblk_request_ctx {
    int in_use;
    uint16_t desc_head;
    struct completion completion;
    struct virtio_blk_req* hdr;
    uint64_t hdr_phys;
    uint8_t* status;
    uint64_t status_phys;
};

static struct vblk_request_ctx g_vblk_requests[VIRTIO_BLK_MAX_REQUESTS];

static int virtio_blk_irq(int irq, void* ctx);

static void vblk_assert_request_ctx(const struct vblk_request_ctx* req) {
    KASSERT(req != 0);
    KASSERT(req->in_use);
    KASSERT(req->hdr != 0);
    KASSERT(req->status != 0);
    KASSERT(req->hdr_phys != 0);
    KASSERT(req->status_phys == req->hdr_phys + sizeof(struct virtio_blk_req));
    KASSERT((req->hdr_phys & (PAGE_SIZE - 1)) == 0);
    KASSERT(req->desc_head + VIRTIO_BLK_DESCS_PER_REQ <= g_vblk_q.queue_size);
    KASSERT((req->desc_head % VIRTIO_BLK_DESCS_PER_REQ) == 0);
}

static int vblk_reclaim_used(void) {
    int completed = 0;
    struct vblk_request_ctx* done_reqs[VIRTIO_BLK_MAX_REQUESTS];
    int done_status[VIRTIO_BLK_MAX_REQUESTS];
    uint64_t flags;
    if (!g_vblk_ready) return -1;
    flags = spin_lock_irqsave(&g_vblk_idle_wait.lock);
    if (g_vblk_q.last_used_idx == g_vblk_q.used->idx) {
        spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
        return 0;
    }
    while (g_vblk_q.last_used_idx != g_vblk_q.used->idx) {
        struct vring_used_elem* elem =
            &g_vblk_q.used->ring[g_vblk_q.last_used_idx % g_vblk_q.queue_size];
        uint16_t head = (uint16_t)elem->id;
        struct vblk_request_ctx* req = 0;
        KASSERT(head < g_vblk_q.queue_size);
        for (int i = 0; i < g_vblk_request_count; i++) {
            if (g_vblk_requests[i].in_use && g_vblk_requests[i].desc_head == head) {
                req = &g_vblk_requests[i];
                break;
            }
        }
        g_vblk_q.last_used_idx++;
        if (!req) continue;
        vblk_assert_request_ctx(req);
        if (completed < VIRTIO_BLK_MAX_REQUESTS) {
            done_reqs[completed] = req;
            done_status[completed] = *req->status == VIRTIO_BLK_S_OK ? 0 : -1;
            completed++;
        }
    }
    spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
    for (int i = 0; i < completed; i++) {
        complete_status(&done_reqs[i]->completion, done_status[i]);
    }
    return completed;
}

static void vblk_complete_bottom_half(void* arg) {
    int status;
    (void)arg;
    status = vblk_reclaim_used();
    if (status == 0) return;
}

static int vblk_has_free_request(void* arg) {
    (void)arg;
    for (int i = 0; i < g_vblk_request_count; i++) {
        if (!g_vblk_requests[i].in_use) return 1;
    }
    return 0;
}

static struct vblk_request_ctx* vblk_acquire_request(void) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_vblk_idle_wait.lock);
        for (int i = 0; i < g_vblk_request_count; i++) {
            if (!g_vblk_requests[i].in_use) {
                struct vblk_request_ctx* req = &g_vblk_requests[i];
                req->in_use = 1;
                req->desc_head = (uint16_t)(i * VIRTIO_BLK_DESCS_PER_REQ);
                reinit_completion(&req->completion);
                *req->status = 0xFF;
                vblk_assert_request_ctx(req);
                spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
                return req;
            }
        }
        spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);

        if (!get_current_task()) {
            while (!vblk_has_free_request(0)) {
                __asm__ volatile("pause");
            }
            continue;
        }
        if (wait_event(&g_vblk_idle_wait, vblk_has_free_request, 0) < 0) {
            return 0;
        }
    }
}

static void vblk_release_request(struct vblk_request_ctx* req) {
    uint64_t flags = spin_lock_irqsave(&g_vblk_idle_wait.lock);
    if (req) {
        vblk_assert_request_ctx(req);
        req->in_use = 0;
    }
    spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
    wake_up_one(&g_vblk_idle_wait);
}

static int vblk_init_request_pool(void) {
    g_vblk_request_count = (int)(g_vblk_q.queue_size / VIRTIO_BLK_DESCS_PER_REQ);
    if (g_vblk_request_count > VIRTIO_BLK_MAX_REQUESTS) {
        g_vblk_request_count = VIRTIO_BLK_MAX_REQUESTS;
    }
    if (g_vblk_request_count <= 0) return -1;

    for (int i = 0; i < g_vblk_request_count; i++) {
        void* req_page_phys = pmm_alloc(1);
        if (!req_page_phys) return -1;
        g_vblk_requests[i].in_use = 0;
        g_vblk_requests[i].desc_head = (uint16_t)(i * VIRTIO_BLK_DESCS_PER_REQ);
        g_vblk_requests[i].hdr = (struct virtio_blk_req*)PHYS_TO_VIRT(req_page_phys);
        g_vblk_requests[i].hdr_phys = (uint64_t)req_page_phys;
        g_vblk_requests[i].status = (uint8_t*)g_vblk_requests[i].hdr + sizeof(struct virtio_blk_req);
        g_vblk_requests[i].status_phys =
            g_vblk_requests[i].hdr_phys + sizeof(struct virtio_blk_req);
        init_completion(&g_vblk_requests[i].completion);
        KASSERT(g_vblk_requests[i].desc_head + VIRTIO_BLK_DESCS_PER_REQ <= g_vblk_q.queue_size);
    }

    return 0;
}

static void vblk_fail(const char* msg) {
    if (g_vblk_iobase) {
        outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_FAILED);
    }
    puts("[vblk] virtio-blk init failed: ");
    puts(msg);
    puts("\r\n");
}

static void vblk_disable_after_timeout(void) {
    g_vblk_ready = 0;
    if (g_vblk_irq_line >= 0) {
        pic_mask_irq(g_vblk_irq_line);
    }
    g_vblk_irq_enabled = 0;
    vblk_fail("request timeout");
}

int virtio_blk_init(void) {
    if (g_vblk_ready) return 0;

    struct pci_device_info dev;
    if (pci_find_virtio_blk(&dev) < 0) {
        return -1;
    }

    g_vblk_iobase = pci_get_bar0_iobase(&dev);
    if (g_vblk_iobase == 0) {
        return -1;
    }

    pci_enable_io_busmaster(&dev);
    wait_queue_init(&g_vblk_idle_wait);
    g_vblk_irq_line = (dev.irq_line <= 15) ? (int)dev.irq_line : -1;
    g_vblk_irq_vector = irq_alloc_vector();

    /* 1. Reset device */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), 0);
    /* 2. ACKNOWLEDGE */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    /* 3. DRIVER */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Feature negotiation (Skip for legacy) */
    (void)inl((uint16_t)(g_vblk_iobase + VIRTIO_PCI_HOST_FEATURES));
    outl((uint16_t)(g_vblk_iobase + VIRTIO_PCI_GUEST_FEATURES), 0);

    /* 5. Initialize Virtqueue */
    if (virtio_virtq_init(g_vblk_iobase, VIRTIO_BLK_QUEUE, &g_vblk_q) < 0) {
        vblk_fail("queue init");
        return -1;
    }

    if (vblk_init_request_pool() < 0) {
        vblk_fail("request pool allocation");
        return -1;
    }

    /* 6. Read capacity from config space */
    struct virtio_blk_config cfg;
    for (size_t i = 0; i < sizeof(cfg); i++) {
        ((uint8_t*)&cfg)[i] = inb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_CONFIG + i));
    }
    g_vblk_capacity = cfg.capacity;

    if (g_vblk_irq_vector >= 0 &&
        irq_register_vector(g_vblk_irq_vector, virtio_blk_irq, 0) == 0 &&
        pci_enable_msi(&dev, (uint8_t)g_vblk_irq_vector) == 0) {
        g_vblk_irq_enabled = 1;
        g_vblk_msi_enabled = 1;
    } else if (g_vblk_irq_vector >= 0 &&
               pci_enable_msix(&dev, (uint8_t)g_vblk_irq_vector, 0) == 0) {
        virtio_disable_config_msix_vector(g_vblk_iobase);
        virtio_set_queue_msix_vector(g_vblk_iobase, VIRTIO_BLK_QUEUE, 0);
        g_vblk_irq_enabled = 1;
        g_vblk_msix_enabled = 1;
    }

    /* 7. DRIVER_OK */
    outb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_vblk_ready = 1;
    if (!g_vblk_irq_enabled && g_vblk_irq_line >= 0) {
        (void)irq_register_legacy(g_vblk_irq_line, virtio_blk_irq, 0);
        pic_unmask_irq(g_vblk_irq_line);
        g_vblk_irq_enabled = 1;
    }
    puts("[vblk] virtio-blk ready: capacity=");
    puthex(g_vblk_capacity);
    puts(" sectors (");
    puthex(g_vblk_capacity * 512 / 1024 / 1024);
    puts(" MiB) io=0x");
    puthex(g_vblk_iobase);
    puts(" irq=0x");
    puthex((uint64_t)(uint32_t)((g_vblk_msi_enabled || g_vblk_msix_enabled) ?
                                g_vblk_irq_vector : g_vblk_irq_line));
    if (g_vblk_msix_enabled) {
        puts(" msix=1");
    } else {
        puts(g_vblk_msi_enabled ? " msi=1" : " msi=0");
    }
    puts(" reqs=0x");
    puthex((uint64_t)(uint32_t)g_vblk_request_count);
    puts("\r\n");

    return 0;
}

static int virtio_blk_irq(int irq, void* ctx) {
    uint8_t isr;
    (void)ctx;
    if (!g_vblk_ready) return 0;
    if (g_vblk_msi_enabled || g_vblk_msix_enabled) {
        if (irq != g_vblk_irq_vector) return 0;
    } else if (irq != g_vblk_irq_line) {
        return 0;
    }
    isr = inb((uint16_t)(g_vblk_iobase + VIRTIO_PCI_ISR));
    if ((isr & 0x1U) == 0) return 1;
    if (bottom_half_enqueue(vblk_complete_bottom_half, 0) < 0) {
        vblk_complete_bottom_half(0);
    }
    return 1;
}

static void vblk_fill_descriptors(struct vblk_request_ctx* req, uint32_t type,
                                  uint64_t sector, void* buf, uint32_t len) {
    vblk_assert_request_ctx(req);
    KASSERT(buf != 0);
    KASSERT(len > 0);
    KASSERT(type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_OUT);
    uint16_t head = req->desc_head;
    uint16_t data = (uint16_t)(head + 1U);
    uint16_t status = (uint16_t)(head + 2U);
    uint64_t data_phys = vmm_get_phys(vmm_get_kernel_pml4(), (uint64_t)(uintptr_t)buf);
    if (data_phys == 0) {
        data_phys = VIRT_TO_PHYS(buf);
    }
    KASSERT(data_phys != 0);

    req->hdr->type = type;
    req->hdr->reserved = 0;
    req->hdr->sector = sector;

    g_vblk_q.desc[head].addr = req->hdr_phys;
    g_vblk_q.desc[head].len = sizeof(struct virtio_blk_req);
    g_vblk_q.desc[head].flags = VRING_DESC_F_NEXT;
    g_vblk_q.desc[head].next = data;

    g_vblk_q.desc[data].addr = data_phys;
    g_vblk_q.desc[data].len = len;
    g_vblk_q.desc[data].flags =
        VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    g_vblk_q.desc[data].next = status;

    g_vblk_q.desc[status].addr = req->status_phys;
    g_vblk_q.desc[status].len = 1;
    g_vblk_q.desc[status].flags = VRING_DESC_F_WRITE;
    g_vblk_q.desc[status].next = 0;
}

static void vblk_submit_request(struct vblk_request_ctx* req) {
    vblk_assert_request_ctx(req);
    uint64_t flags = spin_lock_irqsave(&g_vblk_idle_wait.lock);
    g_vblk_q.avail->ring[g_vblk_q.avail->idx % g_vblk_q.queue_size] = req->desc_head;
    __sync_synchronize();
    g_vblk_q.avail->idx++;
    spin_unlock_irqrestore(&g_vblk_idle_wait.lock, flags);
    virtio_kick(g_vblk_iobase, VIRTIO_BLK_QUEUE);
}

static int vblk_consume_completion_status(struct vblk_request_ctx* req) {
    int status;
    vblk_assert_request_ctx(req);
    uint64_t flags = spin_lock_irqsave(&req->completion.wait.lock);
    if (req->completion.done == 0) {
        spin_unlock_irqrestore(&req->completion.wait.lock, flags);
        return -1;
    }
    req->completion.done--;
    status = req->completion.status;
    spin_unlock_irqrestore(&req->completion.wait.lock, flags);
    return status;
}

static int vblk_wait_request(struct vblk_request_ctx* req) {
    vblk_assert_request_ctx(req);
    if (g_vblk_irq_enabled && get_current_task()) {
        int status = 0;
        int wait_ret = wait_for_completion_timeout_status(&req->completion,
                                                          VIRTIO_BLK_TIMEOUT_MS,
                                                          &status);
        if (wait_ret == 0) {
            vblk_disable_after_timeout();
            return -1;
        }
        if (wait_ret < 0 || status < 0) return -1;
        return 0;
    }

    while (req->completion.done == 0) {
        int status = vblk_reclaim_used();
        if (status < 0) return -1;
        __asm__ volatile("pause");
    }
    return vblk_consume_completion_status(req);
}

static int vblk_request(uint32_t type, uint64_t sector, void* buf, uint32_t len) {
    int ret = -1;
    struct vblk_request_ctx* req;
    if (!g_vblk_ready) return -1;
    req = vblk_acquire_request();
    if (!req) return -1;

    vblk_fill_descriptors(req, type, sector, buf, len);
    vblk_submit_request(req);

    if (vblk_wait_request(req) < 0) {
        goto out;
    }

    ret = 0;
out:
    vblk_release_request(req);
    return ret;
}

int virtio_blk_read(uint64_t sector, void* buf, uint32_t count) {
    return vblk_request(VIRTIO_BLK_T_IN, sector, buf, count * 512);
}

int virtio_blk_write(uint64_t sector, const void* buf, uint32_t count) {
    return vblk_request(VIRTIO_BLK_T_OUT, sector, (void*)buf, count * 512);
}

uint64_t virtio_blk_capacity(void) {
    return g_vblk_capacity;
}

/* ── kout: second virtio-blk, write-only, polling (no IRQ) ─────────────── */

#include "storage.h"

static struct virtio_queue g_kout_q;
static uint16_t g_kout_iobase = 0;
static int g_kout_ready = 0;
static uint64_t g_kout_capacity = 0;
static struct virtio_blk_req* g_kout_hdr = 0;
static uint64_t g_kout_hdr_phys = 0;
static uint8_t* g_kout_status_byte = 0;
static uint64_t g_kout_status_phys = 0;
static uint8_t* g_kout_staging = 0;
static uint64_t g_kout_staging_phys = 0;

static int kout_do_write_sector(uint64_t lba) {
    *g_kout_status_byte = 0xFF;

    g_kout_hdr->type     = VIRTIO_BLK_T_OUT;
    g_kout_hdr->reserved = 0;
    g_kout_hdr->sector   = lba;

    g_kout_q.desc[0].addr  = g_kout_hdr_phys;
    g_kout_q.desc[0].len   = sizeof(struct virtio_blk_req);
    g_kout_q.desc[0].flags = VRING_DESC_F_NEXT;
    g_kout_q.desc[0].next  = 1;

    g_kout_q.desc[1].addr  = g_kout_staging_phys;
    g_kout_q.desc[1].len   = 512;
    g_kout_q.desc[1].flags = VRING_DESC_F_NEXT;
    g_kout_q.desc[1].next  = 2;

    g_kout_q.desc[2].addr  = g_kout_status_phys;
    g_kout_q.desc[2].len   = 1;
    g_kout_q.desc[2].flags = VRING_DESC_F_WRITE;
    g_kout_q.desc[2].next  = 0;

    g_kout_q.avail->ring[g_kout_q.avail->idx % g_kout_q.queue_size] = 0;
    __sync_synchronize();
    g_kout_q.avail->idx++;
    virtio_kick(g_kout_iobase, 0);

    for (int i = 0; i < 4000000; i++) {
        if (g_kout_q.last_used_idx != g_kout_q.used->idx) {
            g_kout_q.last_used_idx++;
            return (*g_kout_status_byte == 0) ? 0 : -1;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

int virtio_kout_write_raw(uint64_t byte_offset, const void* buf, size_t count) {
    const uint8_t* src = (const uint8_t*)buf;
    uint64_t lba = byte_offset / 512;
    size_t sector_pos = (size_t)(byte_offset % 512);
    size_t remaining = count;

    if (!g_kout_ready) return -1;

    while (remaining > 0) {
        size_t chunk = 512 - sector_pos;
        if (chunk > remaining) chunk = remaining;

        if (sector_pos > 0 || chunk < 512) {
            for (int i = 0; i < 512; i++) g_kout_staging[i] = 0;
        }
        for (size_t i = 0; i < chunk; i++)
            g_kout_staging[sector_pos + i] = src[i];

        if (kout_do_write_sector(lba) < 0) return -1;

        src += chunk;
        remaining -= chunk;
        lba++;
        sector_pos = 0;
    }
    return 0;
}

static int kout_storage_read(void* ctx, uint64_t lba, void* buf, size_t count) {
    (void)ctx; (void)lba; (void)buf; (void)count;
    return -1;
}

static int kout_storage_write(void* ctx, uint64_t lba, const void* buf, size_t count) {
    (void)ctx;
    const uint8_t* src = (const uint8_t*)buf;
    for (size_t i = 0; i < count; i++) {
        for (int j = 0; j < 512; j++) g_kout_staging[j] = src[i * 512 + j];
        if (kout_do_write_sector(lba + i) < 0) return -1;
    }
    return 0;
}

int virtio_kout_init(void) {
    struct pci_device_info dev;
    if (pci_find_virtio_blk_n(1, &dev) < 0) return -1;

    g_kout_iobase = pci_get_bar0_iobase(&dev);
    if (!g_kout_iobase) return -1;

    pci_enable_io_busmaster(&dev);

    outb((uint16_t)(g_kout_iobase + VIRTIO_PCI_STATUS), 0);
    outb((uint16_t)(g_kout_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    outb((uint16_t)(g_kout_iobase + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    (void)inl((uint16_t)(g_kout_iobase + VIRTIO_PCI_HOST_FEATURES));
    outl((uint16_t)(g_kout_iobase + VIRTIO_PCI_GUEST_FEATURES), 0);

    if (virtio_virtq_init(g_kout_iobase, 0, &g_kout_q) < 0) return -1;

    void* hdr_phys = pmm_alloc(1);
    if (!hdr_phys) return -1;
    g_kout_hdr       = (struct virtio_blk_req*)PHYS_TO_VIRT(hdr_phys);
    g_kout_hdr_phys  = (uint64_t)hdr_phys;
    g_kout_status_byte  = (uint8_t*)g_kout_hdr + sizeof(struct virtio_blk_req);
    g_kout_status_phys  = g_kout_hdr_phys + sizeof(struct virtio_blk_req);

    void* stg_phys = pmm_alloc(1);
    if (!stg_phys) return -1;
    g_kout_staging      = (uint8_t*)PHYS_TO_VIRT(stg_phys);
    g_kout_staging_phys = (uint64_t)stg_phys;

    struct virtio_blk_config cfg;
    for (size_t i = 0; i < sizeof(cfg); i++)
        ((uint8_t*)&cfg)[i] = inb((uint16_t)(g_kout_iobase + VIRTIO_PCI_CONFIG + i));
    g_kout_capacity = cfg.capacity;

    outb((uint16_t)(g_kout_iobase + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_kout_ready = 1;
    storage_register_device("kout", 512, g_kout_capacity,
                            kout_storage_read, kout_storage_write, NULL, 0);
    puts("[kout] second virtio-blk ready as kout\r\n");
    return 0;
}
