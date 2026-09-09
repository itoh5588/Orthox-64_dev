#include <stdint.h>
#include <stddef.h>
#include "kassert.h"
#include "virtio_net.h"
#include "virtio.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "pic.h"
#include "bottom_half.h"
#include "irq.h"

void puts(const char* s);
void puthex(uint64_t v);

#define VIRTIO_PCI_DEVICE_NET_MIN 0x1000
#define VIRTIO_PCI_DEVICE_NET_MAX 0x107F

#define VIRTIO_RX_QUEUE    0
#define VIRTIO_TX_QUEUE    1
#define VIRTIO_RX_SLOTS    8
#define VIRTIO_TX_SLOT     0
#define VIRTIO_FRAME_MAX   1514
#define VIRTIO_RX_BUF_SIZE 2048

struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

static struct virtio_queue g_rxq;
static struct virtio_queue g_txq;
static uint8_t* g_rx_bufs[VIRTIO_RX_SLOTS];
static uint64_t g_rx_buf_phys[VIRTIO_RX_SLOTS];
static uint8_t* g_tx_buf;
static uint64_t g_tx_buf_phys;
static uint16_t g_iobase;
static int g_irq_line = -1;
static int g_irq_vector = -1;
static uint8_t g_mac[6];
static int g_ready = 0;
static int g_irq_enabled = 0;
static int g_msi_enabled = 0;
static int g_msix_enabled = 0;
static int g_tx_busy = 0;
static volatile int g_irq_bh_pending = 0;
static volatile int g_polling = 0;
static volatile int g_irq_bh_logged = 0;
static virtio_net_rx_cb_t g_rx_cb = 0;

static int virtio_net_irq(int irq, void* ctx);

static void* kernel_memset(void* s, int c, size_t n) {
    uint8_t* p = (uint8_t*)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

static void puthex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char s[3];
    s[0] = hex[(v >> 4) & 0xF];
    s[1] = hex[v & 0xF];
    s[2] = '\0';
    puts(s);
}

static void virtio_net_fail(const char* msg) {
    outb((uint16_t)(g_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_FAILED);
    puts("[net] virtio-net init failed: ");
    puts(msg);
    puts("\r\n");
}

static void virtio_net_assert_queue_ready(const struct virtio_queue* q) {
    KASSERT(q != 0);
    KASSERT(q->queue_size > 0);
    KASSERT(q->desc != 0);
    KASSERT(q->avail != 0);
    KASSERT(q->used != 0);
}

static void virtio_net_reclaim_tx(void) {
    virtio_net_assert_queue_ready(&g_txq);
    while (g_txq.last_used_idx != g_txq.used->idx) {
        struct vring_used_elem* elem = &g_txq.used->ring[g_txq.last_used_idx % g_txq.queue_size];
        KASSERT(elem->id == VIRTIO_TX_SLOT);
        g_txq.last_used_idx++;
        g_tx_busy = 0;
    }
}

static void virtio_net_bottom_half(void* arg) {
    (void)arg;
    if (__sync_bool_compare_and_swap(&g_irq_bh_logged, 0, 1)) {
        puts("[net] virtio-net irq bottom half active\r\n");
    }
    virtio_net_poll();
    __atomic_store_n(&g_irq_bh_pending, 0, __ATOMIC_RELEASE);
}

int virtio_net_init(void) {
    if (g_ready) return 0;

    struct pci_device_info dev;
    if (pci_find_virtio_net(&dev) < 0) {
        return -1;
    }
    if (dev.vendor_id != VIRTIO_PCI_VENDOR_ID ||
        dev.device_id < VIRTIO_PCI_DEVICE_NET_MIN ||
        dev.device_id > VIRTIO_PCI_DEVICE_NET_MAX) {
        return -1;
    }

    g_iobase = pci_get_bar0_iobase(&dev);
    if (g_iobase == 0) {
        return -1;
    }

    pci_enable_io_busmaster(&dev);
    g_irq_line = (dev.irq_line <= 15) ? (int)dev.irq_line : -1;
    g_irq_vector = irq_alloc_vector();

    outb((uint16_t)(g_iobase + VIRTIO_PCI_STATUS), 0);
    outb((uint16_t)(g_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    outb((uint16_t)(g_iobase + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    (void)inl((uint16_t)(g_iobase + VIRTIO_PCI_HOST_FEATURES));
    outl((uint16_t)(g_iobase + VIRTIO_PCI_GUEST_FEATURES), 0);

    if (virtio_virtq_init(g_iobase, VIRTIO_RX_QUEUE, &g_rxq) < 0) {
        virtio_net_fail("rx queue");
        return -1;
    }
    if (virtio_virtq_init(g_iobase, VIRTIO_TX_QUEUE, &g_txq) < 0) {
        virtio_net_fail("tx queue");
        return -1;
    }

    if (g_rxq.queue_size > VIRTIO_RX_SLOTS) {
        g_rxq.active_descs = VIRTIO_RX_SLOTS;
    } else {
        g_rxq.active_descs = g_rxq.queue_size;
    }
    g_txq.active_descs = 1;
    virtio_net_assert_queue_ready(&g_rxq);
    virtio_net_assert_queue_ready(&g_txq);
    KASSERT(g_rxq.active_descs > 0);
    KASSERT(g_rxq.active_descs <= VIRTIO_RX_SLOTS);
    KASSERT(g_rxq.active_descs <= g_rxq.queue_size);
    KASSERT(VIRTIO_TX_SLOT < g_txq.queue_size);

    for (uint16_t i = 0; i < g_rxq.active_descs; i++) {
        void* buf_phys = pmm_alloc(1);
        if (!buf_phys) {
            virtio_net_fail("rx buffers");
            return -1;
        }
        g_rx_buf_phys[i] = (uint64_t)buf_phys;
        KASSERT((g_rx_buf_phys[i] & (PAGE_SIZE - 1)) == 0);
        g_rx_bufs[i] = (uint8_t*)PHYS_TO_VIRT(buf_phys);
        kernel_memset(g_rx_bufs[i], 0, PAGE_SIZE);

        g_rxq.desc[i].addr = g_rx_buf_phys[i];
        g_rxq.desc[i].len = sizeof(struct virtio_net_hdr) + VIRTIO_RX_BUF_SIZE;
        g_rxq.desc[i].flags = VRING_DESC_F_WRITE;
        g_rxq.desc[i].next = 0;
        g_rxq.avail->ring[i] = i;
    }
    g_rxq.avail->idx = g_rxq.active_descs;

    void* tx_phys = pmm_alloc(1);
    if (!tx_phys) {
        virtio_net_fail("tx buffer");
        return -1;
    }
    g_tx_buf_phys = (uint64_t)tx_phys;
    KASSERT((g_tx_buf_phys & (PAGE_SIZE - 1)) == 0);
    g_tx_buf = (uint8_t*)PHYS_TO_VIRT(tx_phys);
    kernel_memset(g_tx_buf, 0, PAGE_SIZE);
    g_txq.desc[VIRTIO_TX_SLOT].addr = g_tx_buf_phys;
    g_txq.desc[VIRTIO_TX_SLOT].len = 0;
    g_txq.desc[VIRTIO_TX_SLOT].flags = 0;
    g_txq.desc[VIRTIO_TX_SLOT].next = 0;

    for (uint16_t i = 0; i < 6; i++) {
        g_mac[i] = inb((uint16_t)(g_iobase + VIRTIO_PCI_CONFIG + i));
    }

    if (g_irq_vector >= 0 &&
        irq_register_vector(g_irq_vector, virtio_net_irq, 0) == 0 &&
        pci_enable_msi(&dev, (uint8_t)g_irq_vector) == 0) {
        g_irq_enabled = 1;
        g_msi_enabled = 1;
    } else if (g_irq_vector >= 0 &&
               pci_enable_msix(&dev, (uint8_t)g_irq_vector, 0) == 0) {
        virtio_disable_config_msix_vector(g_iobase);
        virtio_set_queue_msix_vector(g_iobase, VIRTIO_RX_QUEUE, 0);
        virtio_set_queue_msix_vector(g_iobase, VIRTIO_TX_QUEUE, 0);
        g_irq_enabled = 1;
        g_msix_enabled = 1;
    }

    virtio_kick(g_iobase, VIRTIO_RX_QUEUE);
    outb((uint16_t)(g_iobase + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_ready = 1;
    if (!g_irq_enabled && g_irq_line >= 0) {
        (void)irq_register_legacy(g_irq_line, virtio_net_irq, 0);
        pic_unmask_irq(g_irq_line);
        g_irq_enabled = 1;
    }
    puts("[net] virtio-net ready mac=");
    for (uint16_t i = 0; i < 6; i++) {
        puthex8(g_mac[i]);
        if (i + 1 != 6) puts(":");
    }
    puts(" io=0x");
    puthex(g_iobase);
    puts(" irq=0x");
    puthex((uint64_t)(uint32_t)((g_msi_enabled || g_msix_enabled) ?
                                g_irq_vector : g_irq_line));
    if (g_msix_enabled) {
        puts(" msix=1");
    } else {
        puts(g_msi_enabled ? " msi=1" : " msi=0");
    }
    puts("\r\n");
    return 0;
}

void virtio_net_poll(void) {
    if (!g_ready) return;
    if (__sync_lock_test_and_set(&g_polling, 1)) return;
    virtio_net_assert_queue_ready(&g_rxq);

    virtio_net_reclaim_tx();

    uint16_t requeued = 0;
    while (g_rxq.last_used_idx != g_rxq.used->idx) {
        struct vring_used_elem* elem = &g_rxq.used->ring[g_rxq.last_used_idx % g_rxq.queue_size];
        uint16_t desc_id = (uint16_t)elem->id;
        uint32_t total_len = elem->len;
        uint16_t frame_len = 0;
        KASSERT(desc_id < g_rxq.queue_size);
        if (desc_id < g_rxq.active_descs && total_len > sizeof(struct virtio_net_hdr)) {
            KASSERT(g_rx_bufs[desc_id] != 0);
            frame_len = (uint16_t)(total_len - sizeof(struct virtio_net_hdr));
            if (frame_len > VIRTIO_RX_BUF_SIZE) frame_len = VIRTIO_RX_BUF_SIZE;
            if (g_rx_cb && frame_len > 0) {
                g_rx_cb(g_rx_bufs[desc_id] + sizeof(struct virtio_net_hdr), frame_len);
            }
            g_rxq.avail->ring[g_rxq.avail->idx % g_rxq.queue_size] = desc_id;
            g_rxq.avail->idx++;
            requeued++;
        }
        g_rxq.last_used_idx++;
    }

    if (requeued) {
        virtio_kick(g_iobase, VIRTIO_RX_QUEUE);
    }

    (void)inb((uint16_t)(g_iobase + VIRTIO_PCI_ISR));
    __sync_lock_release(&g_polling);
}

int virtio_net_needs_poll_fallback(void) {
    return g_ready && !g_irq_enabled;
}

static int virtio_net_irq(int irq, void* ctx) {
    uint8_t isr;
    (void)ctx;
    if (!g_ready) return 0;
    if (g_msi_enabled || g_msix_enabled) {
        if (irq != g_irq_vector) return 0;
    } else if (irq != g_irq_line) {
        return 0;
    }
    isr = inb((uint16_t)(g_iobase + VIRTIO_PCI_ISR));
    if ((isr & 0x3U) == 0) return 1;
    if (__sync_bool_compare_and_swap(&g_irq_bh_pending, 0, 1)) {
        if (bottom_half_enqueue(virtio_net_bottom_half, 0) < 0) {
            __atomic_store_n(&g_irq_bh_pending, 0, __ATOMIC_RELEASE);
        }
    }
    return 1;
}

int virtio_net_is_ready(void) {
    return g_ready;
}

int virtio_net_send(const void* frame, uint16_t len) {
    if (!g_ready || !frame || len == 0 || len > VIRTIO_FRAME_MAX) return -1;

    virtio_net_reclaim_tx();
    if (g_tx_busy) return -1;
    virtio_net_assert_queue_ready(&g_txq);
    KASSERT(g_tx_buf != 0);
    KASSERT(VIRTIO_TX_SLOT < g_txq.queue_size);

    struct virtio_net_hdr* hdr = (struct virtio_net_hdr*)g_tx_buf;
    uint8_t* payload = g_tx_buf + sizeof(struct virtio_net_hdr);
    const uint8_t* src = (const uint8_t*)frame;

    kernel_memset(hdr, 0, sizeof(*hdr));
    for (uint16_t i = 0; i < len; i++) {
        payload[i] = src[i];
    }

    g_txq.desc[VIRTIO_TX_SLOT].len = (uint32_t)(sizeof(struct virtio_net_hdr) + len);
    g_txq.avail->ring[g_txq.avail->idx % g_txq.queue_size] = VIRTIO_TX_SLOT;
    g_txq.avail->idx++;
    g_tx_busy = 1;
    virtio_kick(g_iobase, VIRTIO_TX_QUEUE);
    return 0;
}

const uint8_t* virtio_net_mac(void) {
    return g_mac;
}

void virtio_net_set_rx_callback(virtio_net_rx_cb_t cb) {
    g_rx_cb = cb;
}
