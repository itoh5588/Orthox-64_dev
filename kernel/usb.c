#include <stdint.h>
#include "usb.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "lapic.h"

void puts(const char* s);
void puthex(uint64_t v);

static int g_usb_ready = 0;
static int g_usb_mass_ready = 0;
static int g_xhci_rings_ready = 0;
static int g_xhci_cmd_ready = 0;
static int g_xhci_addr_ready = 0;
static int g_xhci_cfg_ready = 0;
static uint8_t g_xhci_slot_id = 0;
static uint8_t g_xhci_port_id = 0;
static int g_usb_desc_ready = 0;
static int g_usb_cfg_desc_ready = 0;
static int g_usb_msc_if_ready = 0;
static int g_usb_msc_bot_ready = 0;
static int g_usb_msc_inquiry_ok = 0;
static int g_usb_msc_capacity_ok = 0;
static uint16_t g_usb_vid = 0;
static uint16_t g_usb_pid = 0;
static uint8_t g_usb_dev_class = 0;
static uint8_t g_usb_dev_subclass = 0;
static uint8_t g_usb_dev_proto = 0;
static uint8_t g_usb_port_speed = 0;
static uint8_t g_usb_msc_if_class = 0;
static uint8_t g_usb_msc_if_subclass = 0;
static uint8_t g_usb_msc_if_proto = 0;
static uint8_t g_usb_cfg_value = 0;
static uint8_t g_usb_msc_if_number = 0;
static uint8_t g_usb_bulk_out_ep = 0;
static uint8_t g_usb_bulk_in_ep = 0;
static uint16_t g_usb_ep0_mps = 64;
static uint16_t g_usb_bulk_out_mps = 0;
static uint16_t g_usb_bulk_in_mps = 0;
static uint8_t g_usb_bulk_out_dci = 0;
static uint8_t g_usb_bulk_in_dci = 0;
static uint32_t g_usb_block_count = 0;
static uint32_t g_usb_block_size = 0;
static uint64_t g_xhci_mmio = 0;
static uint8_t g_xhci_max_ports = 0;
static uint32_t g_xhci_dboff = 0;
static uint32_t g_xhci_rtsoff = 0;

// xHCI runtime objects (single segment per ring for bring-up).
static uint64_t g_dcbaap_phys = 0;
static uint64_t g_cmd_ring_phys = 0;
static uint64_t g_erst_phys = 0;
static uint64_t g_event_ring_phys = 0;
static uint64_t g_scratchpad_array_phys = 0;
static uint32_t g_scratchpad_count = 0;
static uint32_t g_xhci_ctx_size = 32;
static volatile uint8_t* g_cap_regs = 0;
static volatile uint8_t* g_op_regs = 0;
static volatile uint8_t* g_db_regs = 0;
static volatile uint8_t* g_rt_regs = 0;
static uint32_t g_cmd_enqueue_idx = 0;
static uint32_t g_cmd_cycle = 1;
static uint32_t g_evt_dequeue_idx = 0;
static uint32_t g_evt_cycle = 1;
static uint64_t g_input_ctx_phys = 0;
static uint64_t g_output_ctx_phys = 0;
static uint64_t g_ep0_ring_phys = 0;
static uint64_t g_ep0_buf_phys = 0;
static uint64_t g_ep0_cfg_buf_phys = 0;
static uint32_t g_ep0_enqueue_idx = 0;
static uint32_t g_ep0_cycle = 1;
static uint64_t g_bulk_out_ring_phys = 0;
static uint64_t g_bulk_in_ring_phys = 0;
static uint64_t g_bulk_buf_phys = 0;
static uint32_t g_bulk_out_enqueue_idx = 0;
static uint32_t g_bulk_out_cycle = 1;
static uint32_t g_bulk_in_enqueue_idx = 0;
static uint32_t g_bulk_in_cycle = 1;
static uint32_t g_usb_msc_tag = 1;

#define USB_BULK_BUF_PAGES 2
#define USB_BULK_BUF_SIZE (USB_BULK_BUF_PAGES * PAGE_SIZE)
#define USB_BULK_CSW_OFFSET 512U
#define USB_BULK_DATA_OFFSET 1024U

struct usb_msc_cbw {
    uint32_t sig;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} __attribute__((packed));

struct usb_msc_csw {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed));

static inline uint32_t mmio_read32(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void mmio_write32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}

static inline void mmio_write64(volatile uint8_t* base, uint32_t off, uint64_t v) {
    mmio_write32(base, off, (uint32_t)(v & 0xFFFFFFFFU));
    mmio_write32(base, off + 4, (uint32_t)(v >> 32));
}

static void memzero(void* p, uint64_t n) {
    volatile uint8_t* b = (volatile uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void memcopy(void* dst, const void* src, uint64_t n) {
    volatile uint8_t* d = (volatile uint8_t*)dst;
    const volatile uint8_t* s = (const volatile uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static uint8_t xhci_dci_from_epaddr(uint8_t ep_addr) {
    uint8_t ep_num = ep_addr & 0x0F;
    if (ep_num == 0) return 1;
    return (uint8_t)(ep_num * 2U + ((ep_addr & 0x80U) ? 1U : 0U));
}

static uint32_t xhci_ctx_offset_dw(uint8_t dci) {
    uint32_t stride_dw = g_xhci_ctx_size / 4U;
    return stride_dw * (1U + (uint32_t)dci);
}

static void xhci_ring_reset(uint64_t ring_phys) {
    volatile uint32_t* ring = (volatile uint32_t*)PHYS_TO_VIRT((void*)ring_phys);
    memzero((void*)ring, PAGE_SIZE);
    ring[255 * 4 + 0] = (uint32_t)(ring_phys & 0xFFFFFFFFU);
    ring[255 * 4 + 1] = (uint32_t)(ring_phys >> 32);
    ring[255 * 4 + 2] = 0;
    ring[255 * 4 + 3] = (6U << 10) | (1U << 1) | 1U;
}

static void xhci_ring_enqueue_advance(uint32_t* idx, uint32_t* cycle, uint32_t count) {
    while (count--) {
        (*idx)++;
        if (*idx == 255) {
            *idx = 0;
            *cycle ^= 1U;
        }
    }
}

static int xhci_wait_bits(volatile uint8_t* op, uint32_t off, uint32_t mask, uint32_t expected, uint32_t loops) {
    for (uint32_t i = 0; i < loops; i++) {
        if ((mmio_read32(op, off) & mask) == expected) return 0;
    }
    return -1;
}

static int xhci_poll_cmd_completion(uint64_t* out_cmd_ptr, uint8_t* out_cc, uint8_t* out_slot_id, uint32_t loops) {
    if (!g_rt_regs || !out_cmd_ptr || !out_cc || !out_slot_id) return -1;

    volatile uint32_t* ev_ring = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_event_ring_phys);
    const uint32_t intr0 = 0x20;

    for (uint32_t i = 0; i < loops; i++) {
        volatile uint32_t* trb = &ev_ring[g_evt_dequeue_idx * 4];
        uint32_t d0 = trb[0];
        uint32_t d1 = trb[1];
        uint32_t d2 = trb[2];
        uint32_t d3 = trb[3];

        uint32_t c = d3 & 0x1U;
        if (c != g_evt_cycle) {
            continue;
        }

        uint32_t type = (d3 >> 10) & 0x3F;
        if (type == 33) { // Command Completion Event
            *out_cmd_ptr = ((uint64_t)d1 << 32) | d0;
            *out_cc = (uint8_t)((d2 >> 24) & 0xFF);
            *out_slot_id = (uint8_t)((d3 >> 24) & 0xFF);
        } else {
            *out_cmd_ptr = 0;
            *out_cc = 0xFF;
            *out_slot_id = 0;
        }

        g_evt_dequeue_idx++;
        if (g_evt_dequeue_idx >= 256) {
            g_evt_dequeue_idx = 0;
            g_evt_cycle ^= 1U;
        }

        uint64_t erdp = g_event_ring_phys + (uint64_t)g_evt_dequeue_idx * 16ULL;
        mmio_write64(g_rt_regs, intr0 + 0x18, (erdp & ~0xFULL));
        return (type == 33) ? 0 : 1;
    }

    return -1;
}

static uint64_t xhci_cmd_submit(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3_no_cycle) {
    if (!g_db_regs || g_cmd_ring_phys == 0) return 0;
    volatile uint32_t* cmd_ring = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_cmd_ring_phys);

    uint32_t idx = g_cmd_enqueue_idx;
    volatile uint32_t* trb = &cmd_ring[idx * 4];
    trb[0] = d0;
    trb[1] = d1;
    trb[2] = d2;
    trb[3] = d3_no_cycle | (g_cmd_cycle & 1U);
    __asm__ volatile("" : : : "memory");

    uint64_t trb_phys = g_cmd_ring_phys + (uint64_t)idx * 16ULL;

    g_cmd_enqueue_idx++;
    if (g_cmd_enqueue_idx == 255) {
        g_cmd_enqueue_idx = 0;
        g_cmd_cycle ^= 1U;
    }

    // Ring command doorbell (DB0).
    mmio_write32(g_db_regs, 0x00, 0);
    return trb_phys;
}

static int xhci_cmd_noop(void) {
    g_evt_dequeue_idx = 0;
    g_evt_cycle = 1;
    memzero(PHYS_TO_VIRT((void*)g_event_ring_phys), PAGE_SIZE);
    mmio_write64(g_rt_regs, 0x20 + 0x18, (g_event_ring_phys & ~0xFULL));

    uint64_t cmd_ptr = xhci_cmd_submit(0, 0, 0, (23U << 10)); // No-Op Command TRB
    if (!cmd_ptr) return -1;

    for (uint32_t i = 0; i < 8000000; i++) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 1);
        if (r < 0) continue;
        if (r == 0 && ((ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL))) {
            (void)slot;
            return (cc == 1) ? 0 : -2; // 1: Success
        }
        if (r == 0 && cc == 1) {
            puts("[usb] cmd completion ptr mismatch sent=0x");
            puthex(cmd_ptr);
            puts(" got=0x");
            puthex(ev_cmd_ptr);
            puts("\r\n");
            return 0;
        }
    }

    if (g_op_regs && g_rt_regs) {
        uint32_t usbcmd = mmio_read32(g_op_regs, 0x00);
        uint32_t usbsts = mmio_read32(g_op_regs, 0x04);
        uint32_t iman = mmio_read32(g_rt_regs, 0x20 + 0x00);
        uint64_t erdp = ((uint64_t)mmio_read32(g_rt_regs, 0x20 + 0x1C) << 32) |
                        (uint64_t)mmio_read32(g_rt_regs, 0x20 + 0x18);
        volatile uint32_t* ev0 = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_event_ring_phys);
        puts("[usb] noop timeout usbcmd=0x");
        puthex(usbcmd);
        puts(" usbsts=0x");
        puthex(usbsts);
        puts(" iman=0x");
        puthex(iman);
        puts(" erdp=0x");
        puthex(erdp);
        puts("\r\n");
        puts("[usb] evt0 d0=0x");
        puthex(ev0[0]);
        puts(" d1=0x");
        puthex(ev0[1]);
        puts(" d2=0x");
        puthex(ev0[2]);
        puts(" d3=0x");
        puthex(ev0[3]);
        puts("\r\n");
    }
    return -3;
}

static int xhci_cmd_enable_slot(uint8_t* out_slot_id) {
    if (out_slot_id) *out_slot_id = 0;
    uint64_t cmd_ptr = xhci_cmd_submit(0, 0, 0, (9U << 10)); // Enable Slot Command
    if (!cmd_ptr) return -1;

    for (uint32_t i = 0; i < 8000000; i++) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 1);
        if (r < 0) continue;
        if (r == 0 && (ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL)) {
            if (cc != 1) return -2;
            if (out_slot_id) *out_slot_id = slot;
            return (slot != 0) ? 0 : -3;
        }
    }
    return -4;
}

static int xhci_wait_cmd_completion(uint64_t cmd_ptr, uint8_t slot_expect, uint8_t* out_cc, uint8_t* out_slot) {
    for (uint32_t i = 0; i < 8000000; i++) {
        uint64_t ev_cmd_ptr = 0;
        uint8_t cc = 0xFF;
        uint8_t slot = 0;
        int r = xhci_poll_cmd_completion(&ev_cmd_ptr, &cc, &slot, 1);
        if (r < 0) continue;
        if (r == 0 && (ev_cmd_ptr & ~0xFULL) == (cmd_ptr & ~0xFULL)) {
            if (out_cc) *out_cc = cc;
            if (out_slot) *out_slot = slot;
            if (slot_expect && slot != slot_expect) return -2;
            return 0;
        }
    }
    return -1;
}

static int xhci_poll_transfer_event(uint8_t slot_expect, uint8_t ep_expect, uint64_t trb_expect,
                                    uint32_t loops, uint8_t* out_cc, uint32_t* out_residual) {
    if (!g_rt_regs) return -1;
    volatile uint32_t* ev_ring = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_event_ring_phys);
    const uint32_t intr0 = 0x20;
    for (uint32_t i = 0; i < loops; i++) {
        volatile uint32_t* trb = &ev_ring[g_evt_dequeue_idx * 4];
        uint32_t d0 = trb[0];
        uint32_t d1 = trb[1];
        uint32_t d2 = trb[2];
        uint32_t d3 = trb[3];
        uint32_t c = d3 & 0x1U;
        if (c != g_evt_cycle) continue;

        uint32_t type = (d3 >> 10) & 0x3F;
        uint8_t slot = (uint8_t)((d3 >> 24) & 0xFF);
        uint8_t ep = (uint8_t)((d3 >> 16) & 0x1F);
        uint64_t trb_ptr = (((uint64_t)d1 << 32) | d0) & ~0xFULL;

        g_evt_dequeue_idx++;
        if (g_evt_dequeue_idx >= 256) {
            g_evt_dequeue_idx = 0;
            g_evt_cycle ^= 1U;
        }
        uint64_t erdp = g_event_ring_phys + (uint64_t)g_evt_dequeue_idx * 16ULL;
        mmio_write64(g_rt_regs, intr0 + 0x18, (erdp & ~0xFULL));

        if (type != 32) continue; // Transfer Event
        if (slot_expect && slot != slot_expect) continue;
        if (ep_expect && ep != ep_expect) continue;
        if (trb_expect && trb_ptr != (trb_expect & ~0xFULL)) continue;

        if (out_cc) *out_cc = (uint8_t)((d2 >> 24) & 0xFF);
        if (out_residual) *out_residual = (d2 & 0x00FFFFFFU);
        return 0;
    }
    return -1;
}

static int xhci_cmd_address_device(uint8_t slot_id, uint8_t port_id) {
    if (slot_id == 0 || port_id == 0) return -1;
    if (g_xhci_ctx_size != 32 && g_xhci_ctx_size != 64) return -2;

    void* in_ctx = pmm_alloc(1);
    void* out_ctx = pmm_alloc(1);
    void* ep0_ring = pmm_alloc(1);
    if (!in_ctx || !out_ctx || !ep0_ring) return -3;

    g_input_ctx_phys = (uint64_t)in_ctx;
    g_output_ctx_phys = (uint64_t)out_ctx;
    g_ep0_ring_phys = (uint64_t)ep0_ring;
    if (!g_ep0_buf_phys) {
        void* ep0_buf = pmm_alloc(1);
        if (!ep0_buf) return -3;
        memzero(PHYS_TO_VIRT(ep0_buf), PAGE_SIZE);
        g_ep0_buf_phys = (uint64_t)ep0_buf;
    }

    memzero(PHYS_TO_VIRT(in_ctx), PAGE_SIZE);
    memzero(PHYS_TO_VIRT(out_ctx), PAGE_SIZE);
    memzero(PHYS_TO_VIRT(ep0_ring), PAGE_SIZE);

    // EP0 transfer ring: set Link TRB at tail to make a cycle ring.
    volatile uint32_t* ep = (volatile uint32_t*)PHYS_TO_VIRT(ep0_ring);
    ep[255 * 4 + 0] = (uint32_t)(g_ep0_ring_phys & 0xFFFFFFFFU);
    ep[255 * 4 + 1] = (uint32_t)(g_ep0_ring_phys >> 32);
    ep[255 * 4 + 2] = 0;
    ep[255 * 4 + 3] = (6U << 10) | (1U << 1) | 1U;
    g_ep0_enqueue_idx = 0;
    g_ep0_cycle = 1;

    // DCBAA[slot_id] -> output device context.
    volatile uint64_t* dcbaa = (volatile uint64_t*)PHYS_TO_VIRT((void*)g_dcbaap_phys);
    dcbaa[slot_id] = g_output_ctx_phys;

    volatile uint32_t* ic = (volatile uint32_t*)PHYS_TO_VIRT(in_ctx);
    uint32_t stride_dw = g_xhci_ctx_size / 4U;
    // Input Control Context: Add Slot + EP0 context.
    ic[1] = 0x00000003U;

    // Slot Context (offset = 1 context).
    uint32_t slot_off = stride_dw * 1U;
    uint32_t portsc = mmio_read32(g_op_regs, 0x400 + ((uint32_t)port_id - 1U) * 0x10);
    uint32_t speed = (portsc >> 10) & 0xF;
    if (speed == 0) speed = 3; // fall back to high-speed profile for qemu usb-storage path
    g_usb_port_speed = (uint8_t)speed;
    g_usb_ep0_mps = (speed >= 4) ? 512U : 64U;
    ic[slot_off + 0] = (speed << 20) | (1U << 27);         // SPEED + CONTEXT_ENTRIES = 1 (EP0)
    ic[slot_off + 1] = ((uint32_t)port_id << 16);          // ROOT_HUB_PORT_NUM
    ic[slot_off + 2] = 0;
    ic[slot_off + 3] = 0;

    // EP0 Context (offset = 2 contexts, DCI=1).
    uint32_t ep0_off = stride_dw * 2U;
    ic[ep0_off + 0] = (0U << 16);                          // EP_STATE disabled for input
    // EP_TYPE=Control(4) at bits[5:3], MPS depends on negotiated port speed.
    ic[ep0_off + 1] = (4U << 3) | ((uint32_t)g_usb_ep0_mps << 16);
    ic[ep0_off + 2] = (uint32_t)((g_ep0_ring_phys & ~0xFULL) | 1U); // TR Dequeue + DCS
    ic[ep0_off + 3] = (uint32_t)(g_ep0_ring_phys >> 32);
    ic[ep0_off + 4] = 0;

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (11U << 10) | ((uint32_t)slot_id << 24) // Address Device Command
    );
    if (!cmd_ptr) return -4;

    uint8_t cc = 0xFF;
    uint8_t slot = 0;
    int w = xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot);
    if (w < 0) return -5;
    (void)slot;
    return (cc == 1) ? 0 : -6;
}

static int xhci_cmd_configure_endpoint(uint8_t slot_id) {
    if (slot_id == 0 || g_input_ctx_phys == 0) return -1;

    volatile uint32_t* ic = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_input_ctx_phys);
    // For Configure Endpoint, A1 must stay clear; EP0 context does not apply.
    ic[0] = 0;           // Drop flags
    ic[1] = 0x00000001U; // Add flags: slot only

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (12U << 10) | ((uint32_t)slot_id << 24) // Configure Endpoint Command
    );
    if (!cmd_ptr) return -2;

    uint8_t cc = 0xFF;
    uint8_t slot = 0;
    int w = xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot);
    if (w < 0) return -3;
    (void)slot;
    return (cc == 1) ? 0 : -4;
}

static int xhci_ep0_get_device_descriptor(uint8_t slot_id) {
    if (slot_id == 0 || g_ep0_ring_phys == 0 || g_ep0_buf_phys == 0) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_ep0_ring_phys);
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cycle = g_ep0_cycle & 1U;

    // USB Device Descriptor request (18 bytes):
    // bmRequestType=0x80, bRequest=GET_DESCRIPTOR(6), wValue=0x0100, wIndex=0, wLength=18
    uint64_t setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0100 << 16;
    setup |= (uint64_t)0 << 32;
    setup |= (uint64_t)18 << 48;

    // Setup Stage TRB (TRT=IN => 3)
    ep[idx * 4 + 0] = (uint32_t)(setup & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(setup >> 32);
    ep[idx * 4 + 2] = 8; // setup packet is 8 bytes
    ep[idx * 4 + 3] = (2U << 10) | (1U << 6) | (3U << 16) | (1U << 4) | cycle; // Setup, IDT, TRT=IN, CHAIN

    // Data Stage TRB (IN, 18 bytes)
    idx++;
    ep[idx * 4 + 0] = (uint32_t)(g_ep0_buf_phys & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(g_ep0_buf_phys >> 32);
    ep[idx * 4 + 2] = 18;
    ep[idx * 4 + 3] = (3U << 10) | (1U << 16) | (1U << 4) | cycle; // Data IN, CHAIN

    // Status Stage TRB (OUT status, IOC)
    idx++;
    ep[idx * 4 + 0] = 0;
    ep[idx * 4 + 1] = 0;
    ep[idx * 4 + 2] = 0;
    ep[idx * 4 + 3] = (4U << 10) | (1U << 5) | cycle; // Status, IOC

    // Ring EP0 doorbell (DB[slot], target endpoint 1)
    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    xhci_ring_enqueue_advance(&g_ep0_enqueue_idx, &g_ep0_cycle, 3);

    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) return -3;

    volatile uint8_t* d = (volatile uint8_t*)PHYS_TO_VIRT((void*)g_ep0_buf_phys);
    if (d[1] != 1) return -4; // bDescriptorType must be DEVICE

    g_usb_vid = (uint16_t)d[8] | ((uint16_t)d[9] << 8);
    g_usb_pid = (uint16_t)d[10] | ((uint16_t)d[11] << 8);
    g_usb_dev_class = d[4];
    g_usb_dev_subclass = d[5];
    g_usb_dev_proto = d[6];
    g_usb_desc_ready = 1;
    (void)residual;
    return 0;
}

static int xhci_ep0_control_in(uint8_t slot_id, uint64_t setup, uint64_t data_phys, uint32_t len) {
    if (slot_id == 0 || g_ep0_ring_phys == 0 || data_phys == 0 || len == 0 || len > 4096) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_ep0_ring_phys);
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cycle = g_ep0_cycle & 1U;

    ep[idx * 4 + 0] = (uint32_t)(setup & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(setup >> 32);
    ep[idx * 4 + 2] = 8;
    ep[idx * 4 + 3] = (2U << 10) | (1U << 6) | (3U << 16) | (1U << 4) | cycle; // Setup, IDT, TRT=IN, CHAIN

    idx++;
    ep[idx * 4 + 0] = (uint32_t)(data_phys & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(data_phys >> 32);
    ep[idx * 4 + 2] = len;
    ep[idx * 4 + 3] = (3U << 10) | (1U << 16) | (1U << 4) | cycle; // Data IN, CHAIN

    idx++;
    ep[idx * 4 + 0] = 0;
    ep[idx * 4 + 1] = 0;
    ep[idx * 4 + 2] = 0;
    ep[idx * 4 + 3] = (4U << 10) | (1U << 5) | cycle; // Status + IOC

    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    xhci_ring_enqueue_advance(&g_ep0_enqueue_idx, &g_ep0_cycle, 3);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] ep0 IN cc=0x");
        puthex(cc);
        puts(" residual=0x");
        puthex(residual);
        puts(" len=0x");
        puthex(len);
        puts(" setup=0x");
        puthex(setup);
        puts("\r\n");
        return -3;
    }
    return 0;
}

static int xhci_ep0_control_no_data(uint8_t slot_id, uint64_t setup) {
    if (slot_id == 0 || g_ep0_ring_phys == 0) return -1;

    volatile uint32_t* ep = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_ep0_ring_phys);
    uint32_t idx = g_ep0_enqueue_idx;
    uint32_t cycle = g_ep0_cycle & 1U;

    ep[idx * 4 + 0] = (uint32_t)(setup & 0xFFFFFFFFU);
    ep[idx * 4 + 1] = (uint32_t)(setup >> 32);
    ep[idx * 4 + 2] = 8;
    ep[idx * 4 + 3] = (2U << 10) | (1U << 6) | (1U << 4) | cycle; // Setup, IDT, TRT=No Data, CHAIN

    idx++;
    ep[idx * 4 + 0] = 0;
    ep[idx * 4 + 1] = 0;
    ep[idx * 4 + 2] = 0;
    ep[idx * 4 + 3] = (4U << 10) | (1U << 5) | (1U << 16) | cycle; // Status IN + IOC

    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, 1U);
    xhci_ring_enqueue_advance(&g_ep0_enqueue_idx, &g_ep0_cycle, 2);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, 1, 0, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] ep0 no-data cc=0x");
        puthex(cc);
        puts(" residual=0x");
        puthex(residual);
        puts(" setup=0x");
        puthex(setup);
        puts("\r\n");
        return -3;
    }
    return 0;
}

static uint64_t usb_setup_packet(uint8_t bm_request_type, uint8_t b_request,
                                 uint16_t w_value, uint16_t w_index, uint16_t w_length) {
    uint64_t setup = 0;
    setup |= (uint64_t)bm_request_type;
    setup |= (uint64_t)b_request << 8;
    setup |= (uint64_t)w_value << 16;
    setup |= (uint64_t)w_index << 32;
    setup |= (uint64_t)w_length << 48;
    return setup;
}

static int xhci_ep0_get_config_descriptor(uint8_t slot_id) {
    if (slot_id == 0) return -1;
    if (!g_ep0_cfg_buf_phys) {
        void* b = pmm_alloc(1);
        if (!b) return -2;
        memzero(PHYS_TO_VIRT(b), PAGE_SIZE);
        g_ep0_cfg_buf_phys = (uint64_t)b;
    }

    volatile uint8_t* cfg = (volatile uint8_t*)PHYS_TO_VIRT((void*)g_ep0_cfg_buf_phys);
    memzero((void*)cfg, PAGE_SIZE);

    // 1) First read config header (9 bytes) to get wTotalLength.
    uint64_t setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0200 << 16;
    setup |= (uint64_t)9 << 48;
    {
        int rc = xhci_ep0_control_in(slot_id, setup, g_ep0_cfg_buf_phys, 9);
        if (rc < 0) {
            puts("[usb] GET_DESCRIPTOR(Config hdr) rc=0x");
            puthex((uint64_t)(uint32_t)(-rc));
            puts("\r\n");
            return -3;
        }
    }
    if (cfg[1] != 2) return -4;
    uint16_t total_len = (uint16_t)cfg[2] | ((uint16_t)cfg[3] << 8);
    if (total_len < 9) return -5;
    if (total_len > 255) total_len = 255;

    // 2) Read full configuration descriptor set.
    setup = 0;
    setup |= 0x80ULL;
    setup |= (uint64_t)6 << 8;
    setup |= (uint64_t)0x0200 << 16;
    setup |= (uint64_t)total_len << 48;
    {
        int rc = xhci_ep0_control_in(slot_id, setup, g_ep0_cfg_buf_phys, total_len);
        if (rc < 0) {
            puts("[usb] GET_DESCRIPTOR(Config body) rc=0x");
            puthex((uint64_t)(uint32_t)(-rc));
            puts(" total=0x");
            puthex(total_len);
            puts("\r\n");
            return -6;
        }
    }
    if (cfg[1] != 2) return -7;

    g_usb_cfg_desc_ready = 1;
    g_usb_msc_if_ready = 0;
    g_usb_cfg_value = cfg[5];
    g_usb_msc_if_class = 0;
    g_usb_msc_if_subclass = 0;
    g_usb_msc_if_proto = 0;
    g_usb_msc_if_number = 0;
    g_usb_bulk_out_ep = 0;
    g_usb_bulk_in_ep = 0;
    g_usb_bulk_out_mps = 0;
    g_usb_bulk_in_mps = 0;
    uint8_t current_if_num = 0xFF;
    int current_if_is_msc = 0;

    uint16_t off = 0;
    while (off + 2 <= total_len) {
        uint8_t len = cfg[off + 0];
        uint8_t typ = cfg[off + 1];
        if (len < 2) break;
        if (off + len > total_len) break;
        if (typ == 4 && len >= 9) {
            current_if_num = cfg[off + 2];
            uint8_t c = cfg[off + 5];
            uint8_t s = cfg[off + 6];
            uint8_t p = cfg[off + 7];
            current_if_is_msc = 0;
            if (c == 0x08 && s == 0x06 && p == 0x50) {
                g_usb_msc_if_ready = 1;
                g_usb_msc_if_class = c;
                g_usb_msc_if_subclass = s;
                g_usb_msc_if_proto = p;
                g_usb_msc_if_number = current_if_num;
                current_if_is_msc = 1;
            }
        } else if (typ == 5 && len >= 7 && current_if_is_msc) {
            uint8_t ep_addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3] & 0x03U;
            uint16_t mps = (uint16_t)cfg[off + 4] | ((uint16_t)cfg[off + 5] << 8);
            if (attr == 2) { // bulk
                if (ep_addr & 0x80U) {
                    g_usb_bulk_in_ep = ep_addr;
                    g_usb_bulk_in_mps = mps;
                } else {
                    g_usb_bulk_out_ep = ep_addr;
                    g_usb_bulk_out_mps = mps;
                }
            }
        }
        off += len;
    }
    if (!g_usb_msc_if_ready) return -8;
    if (g_usb_bulk_out_ep == 0 || g_usb_bulk_in_ep == 0) return -9;

    g_usb_bulk_out_dci = xhci_dci_from_epaddr(g_usb_bulk_out_ep);
    g_usb_bulk_in_dci = xhci_dci_from_epaddr(g_usb_bulk_in_ep);
    return 0;
}

static int xhci_setup_bulk_endpoints(uint8_t slot_id) {
    if (slot_id == 0 || g_input_ctx_phys == 0 || g_output_ctx_phys == 0) return -1;
    if (g_usb_bulk_out_dci == 0 || g_usb_bulk_in_dci == 0) return -2;

    if (!g_bulk_out_ring_phys) {
        void* p = pmm_alloc(1);
        if (!p) return -3;
        g_bulk_out_ring_phys = (uint64_t)p;
    }
    if (!g_bulk_in_ring_phys) {
        void* p = pmm_alloc(1);
        if (!p) return -4;
        g_bulk_in_ring_phys = (uint64_t)p;
    }
    if (!g_bulk_buf_phys) {
        void* p = pmm_alloc(USB_BULK_BUF_PAGES);
        if (!p) return -5;
        g_bulk_buf_phys = (uint64_t)p;
    }
    xhci_ring_reset(g_bulk_out_ring_phys);
    xhci_ring_reset(g_bulk_in_ring_phys);
    g_bulk_out_enqueue_idx = 0;
    g_bulk_out_cycle = 1;
    g_bulk_in_enqueue_idx = 0;
    g_bulk_in_cycle = 1;
    memzero(PHYS_TO_VIRT((void*)g_bulk_buf_phys), USB_BULK_BUF_SIZE);

    volatile uint32_t* ic = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_input_ctx_phys);
    memzero((void*)ic, PAGE_SIZE);
    // Input context begins with Input Control Context. Copy the current device
    // context payload after it so controller sees current slot/EP0 state plus our adds.
    {
        uint32_t stride_dw = g_xhci_ctx_size / 4U;
        volatile uint32_t* oc = (volatile uint32_t*)PHYS_TO_VIRT((void*)g_output_ctx_phys);
        for (uint32_t dci = 0; dci <= 31; dci++) {
            uint32_t in_off = stride_dw * (1U + dci);
            uint32_t out_off = stride_dw * dci;
            for (uint32_t j = 0; j < stride_dw; j++) {
                ic[in_off + j] = oc[out_off + j];
            }
        }
    }
    ic[0] = 0;
    // For Configure Endpoint, A0 identifies the slot context; A1 must remain 0.
    ic[1] = (1U << 0) | (1U << g_usb_bulk_out_dci) | (1U << g_usb_bulk_in_dci);

    uint8_t max_dci = (g_usb_bulk_out_dci > g_usb_bulk_in_dci) ? g_usb_bulk_out_dci : g_usb_bulk_in_dci;
    uint32_t slot_off = xhci_ctx_offset_dw(0);
    ic[slot_off + 0] = (ic[slot_off + 0] & ~(0x1FU << 27)) | ((uint32_t)max_dci << 27);

    {
        uint32_t off = xhci_ctx_offset_dw(1);
        ic[off + 0] = 0;
        ic[off + 1] = (4U << 3) | ((uint32_t)g_usb_ep0_mps << 16);
        ic[off + 2] = (uint32_t)((g_ep0_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_ep0_ring_phys >> 32);
        ic[off + 4] = 8;
    }

    {
        uint32_t off = xhci_ctx_offset_dw(g_usb_bulk_out_dci);
        ic[off + 0] = 0;
        ic[off + 1] = (2U << 3) | (3U << 1) | ((uint32_t)g_usb_bulk_out_mps << 16);
        ic[off + 2] = (uint32_t)((g_bulk_out_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_bulk_out_ring_phys >> 32);
        ic[off + 4] = g_usb_bulk_out_mps;
    }

    {
        uint32_t off = xhci_ctx_offset_dw(g_usb_bulk_in_dci);
        ic[off + 0] = 0;
        ic[off + 1] = (6U << 3) | (3U << 1) | ((uint32_t)g_usb_bulk_in_mps << 16);
        ic[off + 2] = (uint32_t)((g_bulk_in_ring_phys & ~0xFULL) | 1U);
        ic[off + 3] = (uint32_t)(g_bulk_in_ring_phys >> 32);
        ic[off + 4] = g_usb_bulk_in_mps;
    }

    uint64_t cmd_ptr = xhci_cmd_submit(
        (uint32_t)(g_input_ctx_phys & 0xFFFFFFFFU),
        (uint32_t)(g_input_ctx_phys >> 32),
        0,
        (12U << 10) | ((uint32_t)slot_id << 24)
    );
    if (!cmd_ptr) return -6;
    uint8_t cc = 0xFF;
    uint8_t slot = 0;
    int w = xhci_wait_cmd_completion(cmd_ptr, slot_id, &cc, &slot);
    if (w < 0) return -7;
    (void)slot;
    if (cc != 1) {
        puts("[usb] Configure Endpoint(BULK) cc=0x");
        puthex(cc);
        puts("\r\n");
    }
    return (cc == 1) ? 0 : -8;
}

static int xhci_bulk_transfer_ex(uint8_t slot_id, uint8_t dci, uint64_t ring_phys, uint64_t data_phys,
                                 uint32_t len, int in_dir, int log_errors) {
    if (slot_id == 0 || dci == 0 || ring_phys == 0 || data_phys == 0) return -1;

    volatile uint32_t* ring = (volatile uint32_t*)PHYS_TO_VIRT((void*)ring_phys);
    uint32_t* idx = in_dir ? &g_bulk_in_enqueue_idx : &g_bulk_out_enqueue_idx;
    uint32_t* cycle = in_dir ? &g_bulk_in_cycle : &g_bulk_out_cycle;
    uint32_t trb = *idx;
    uint32_t pcs = *cycle & 1U;
    uint64_t trb_phys = ring_phys + (uint64_t)trb * 16ULL;
    ring[trb * 4 + 0] = (uint32_t)(data_phys & 0xFFFFFFFFU);
    ring[trb * 4 + 1] = (uint32_t)(data_phys >> 32);
    ring[trb * 4 + 2] = len;
    ring[trb * 4 + 3] = (1U << 10) | (1U << 5) | pcs; // Normal TRB + IOC + cycle

    mmio_write32(g_db_regs, (uint32_t)slot_id * 4U, dci);
    xhci_ring_enqueue_advance(idx, cycle, 1);
    uint8_t cc = 0xFF;
    uint32_t residual = 0;
    if (xhci_poll_transfer_event(slot_id, dci, trb_phys, 8000000, &cc, &residual) < 0) return -2;
    if (cc != 1) {
        puts("[usb] bulk ");
        puts(in_dir ? "IN" : "OUT");
        puts(" dci=0x");
        puthex(dci);
        puts(" cc=0x");
        puthex(cc);
        puts(" residual=0x");
        puthex(residual);
        puts(" len=0x");
        puthex(len);
        puts("\r\n");
    }
    return (cc == 1) ? 0 : -3;
}

static int usb_msc_reset_recovery(void) {
    if (g_xhci_slot_id == 0 || !g_usb_msc_if_ready) return -1;
    if (xhci_ep0_control_no_data(g_xhci_slot_id,
                                 usb_setup_packet(0x21, 0xFF, 0, g_usb_msc_if_number, 0)) < 0) {
        return -2;
    }
    if (g_usb_bulk_out_ep) {
        if (xhci_ep0_control_no_data(g_xhci_slot_id,
                                     usb_setup_packet(0x02, 0x01, 0, g_usb_bulk_out_ep, 0)) < 0) {
            return -3;
        }
    }
    if (g_usb_bulk_in_ep) {
        if (xhci_ep0_control_no_data(g_xhci_slot_id,
                                     usb_setup_packet(0x02, 0x01, 0, g_usb_bulk_in_ep, 0)) < 0) {
            return -4;
        }
    }
    return 0;
}

static int usb_msc_recover_bulk_path(void) {
    if (g_xhci_slot_id == 0) return -1;
    if (g_usb_bulk_out_dci == 0 || g_usb_bulk_in_dci == 0) return -2;
    (void)usb_msc_reset_recovery();
    return xhci_setup_bulk_endpoints(g_xhci_slot_id);
}

static int usb_msc_refresh_bulk_rings_if_needed(void) {
    if (g_xhci_slot_id == 0) return -1;
    return xhci_setup_bulk_endpoints(g_xhci_slot_id);
}

static int usb_msc_bot_command(const uint8_t* cdb, uint8_t cdb_len, void* data, uint32_t data_len, int data_in) {
    if (!g_usb_msc_if_ready || g_xhci_slot_id == 0) return -1;
    if (!cdb || cdb_len == 0 || cdb_len > 16) return -2;
    if (data_len > USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET) return -3;
    if (!g_bulk_buf_phys || !g_bulk_out_ring_phys || !g_bulk_in_ring_phys) return -4;
    if (g_usb_bulk_out_dci == 0 || g_usb_bulk_in_dci == 0) return -5;
    if (usb_msc_refresh_bulk_rings_if_needed() < 0) return -6;

    for (int attempt = 0; attempt < 2; attempt++) {
        volatile uint8_t* buf = (volatile uint8_t*)PHYS_TO_VIRT((void*)g_bulk_buf_phys);
        memzero((void*)buf, USB_BULK_BUF_SIZE);

        struct usb_msc_cbw* cbw = (struct usb_msc_cbw*)((void*)buf);
        struct usb_msc_csw* csw = (struct usb_msc_csw*)((void*)(buf + USB_BULK_CSW_OFFSET));
        cbw->sig = 0x43425355U;
        cbw->tag = g_usb_msc_tag++;
        if (g_usb_msc_tag == 0) g_usb_msc_tag = 1;
        cbw->data_len = data_len;
        cbw->flags = data_in ? 0x80U : 0x00U;
        cbw->lun = 0;
        cbw->cb_len = cdb_len;
        for (uint8_t i = 0; i < cdb_len; i++) cbw->cb[i] = cdb[i];

        int final_attempt = (attempt == 1);
        if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_out_dci, g_bulk_out_ring_phys, g_bulk_buf_phys, 31, 0, final_attempt) < 0) {
            if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
            return -7;
        }

        if (data_len > 0) {
            if (data_in) {
                memzero((void*)(buf + USB_BULK_DATA_OFFSET), USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET);
                if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_in_dci, g_bulk_in_ring_phys, g_bulk_buf_phys + USB_BULK_DATA_OFFSET,
                                          data_len, 1, final_attempt) < 0) {
                    if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
                    return -8;
                }
                if (data) memcopy(data, (const void*)(buf + USB_BULK_DATA_OFFSET), data_len);
            } else {
                if (data) memcopy((void*)(buf + USB_BULK_DATA_OFFSET), data, data_len);
                if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_out_dci, g_bulk_out_ring_phys, g_bulk_buf_phys + USB_BULK_DATA_OFFSET,
                                          data_len, 0, final_attempt) < 0) {
                    if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
                    return -9;
                }
            }
        }

        memzero((void*)csw, sizeof(*csw));
        if (xhci_bulk_transfer_ex(g_xhci_slot_id, g_usb_bulk_in_dci, g_bulk_in_ring_phys, g_bulk_buf_phys + USB_BULK_CSW_OFFSET,
                                  13, 1, final_attempt) < 0) {
            if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
            return -10;
        }
        if (csw->sig != 0x53425355U || csw->tag != cbw->tag || csw->status != 0) {
            if (attempt == 0 && usb_msc_recover_bulk_path() == 0) continue;
            if (csw->sig != 0x53425355U) return -11;
            if (csw->tag != cbw->tag) return -12;
            return -13;
        }

        // Delay to prevent polling starvation / overwhelming the controller
        uint64_t start_tick = lapic_get_ticks_ms();
        while (lapic_get_ticks_ms() - start_tick < 1) {
            __asm__ volatile("pause");
        }

        return 0;
    }
    return -14;
}

static int usb_msc_scsi_inquiry(void) {
    uint8_t cdb[6];
    uint8_t resp[36];
    memzero(cdb, sizeof(cdb));
    memzero(resp, sizeof(resp));
    cdb[0] = 0x12; // INQUIRY
    cdb[4] = sizeof(resp);
    int r = usb_msc_bot_command(cdb, 6, resp, sizeof(resp), 1);
    if (r < 0) return r;
    g_usb_msc_inquiry_ok = 1;
    return 0;
}

static int usb_msc_scsi_read_capacity10(void) {
    uint8_t cdb[10];
    uint8_t resp[8];
    memzero(cdb, sizeof(cdb));
    memzero(resp, sizeof(resp));
    cdb[0] = 0x25; // READ CAPACITY(10)
    int r = usb_msc_bot_command(cdb, 10, resp, sizeof(resp), 1);
    if (r < 0) return r;
    g_usb_block_count =
        ((uint32_t)resp[0] << 24) |
        ((uint32_t)resp[1] << 16) |
        ((uint32_t)resp[2] << 8) |
        (uint32_t)resp[3];
    g_usb_block_count += 1U;
    g_usb_block_size =
        ((uint32_t)resp[4] << 24) |
        ((uint32_t)resp[5] << 16) |
        ((uint32_t)resp[6] << 8) |
        (uint32_t)resp[7];
    g_usb_msc_capacity_ok = (g_usb_block_size != 0);
    return g_usb_msc_capacity_ok ? 0 : -1;
}

static int usb_msc_scsi_test_unit_ready(void) {
    uint8_t cdb[6];
    memzero(cdb, sizeof(cdb));
    cdb[0] = 0x00; // TEST UNIT READY
    return usb_msc_bot_command(cdb, 6, 0, 0, 1);
}

static int usb_msc_scsi_read10(uint32_t lba, void* data, uint32_t block_count) {
    uint8_t cdb[10];
    uint32_t data_len;
    if (!data || block_count == 0) return -1;
    if (!g_usb_msc_capacity_ok || g_usb_block_size == 0) return -2;
    data_len = block_count * g_usb_block_size;
    if (block_count > 0xFFFFU) return -3;
    if (data_len > USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET) return -4;

    memzero(cdb, sizeof(cdb));
    cdb[0] = 0x28; // READ(10)
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)(lba);
    cdb[7] = (uint8_t)(block_count >> 8);
    cdb[8] = (uint8_t)(block_count);
    return usb_msc_bot_command(cdb, 10, data, data_len, 1);
}

static int usb_try_qemu_bulk_fallback(uint8_t slot_id) {
    static const struct {
        uint8_t out_ep;
        uint8_t in_ep;
    } candidates[] = {
        { 0x01, 0x81 },
        { 0x01, 0x82 },
        { 0x02, 0x83 },
        { 0x02, 0x81 },
        { 0x02, 0x82 },
    };

    for (uint32_t i = 0; i < (uint32_t)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        g_usb_bulk_out_ep = candidates[i].out_ep;
        g_usb_bulk_in_ep = candidates[i].in_ep;
        if (g_usb_port_speed >= 4) {
            g_usb_bulk_out_mps = 1024;
            g_usb_bulk_in_mps = 1024;
        } else {
            g_usb_bulk_out_mps = 512;
            g_usb_bulk_in_mps = 512;
        }
        g_usb_bulk_out_dci = xhci_dci_from_epaddr(g_usb_bulk_out_ep);
        g_usb_bulk_in_dci = xhci_dci_from_epaddr(g_usb_bulk_in_ep);
        puts("[usb] trying QEMU bulk pair out=0x");
        puthex(g_usb_bulk_out_ep);
        puts(" in=0x");
        puthex(g_usb_bulk_in_ep);
        puts("\r\n");
        if (xhci_setup_bulk_endpoints(slot_id) == 0) {
            g_usb_msc_inquiry_ok = 0;
            g_usb_msc_capacity_ok = 0;
            if (usb_msc_scsi_inquiry() == 0 && usb_msc_scsi_read_capacity10() == 0) {
                return 0;
            }
        }
    }
    return -1;
}

static int xhci_setup_rings(volatile uint8_t* cap, volatile uint8_t* op, uint32_t hcsparams1, uint32_t hcsparams2) {
    const uint32_t max_slots = hcsparams1 & 0xFF;

    // 1) Allocate and clear DCBAA.
    void* dcbaa_page = pmm_alloc(1);
    if (!dcbaa_page) return -1;
    memzero(PHYS_TO_VIRT(dcbaa_page), PAGE_SIZE);
    g_dcbaap_phys = (uint64_t)dcbaa_page;
    volatile uint64_t* dcbaa = (volatile uint64_t*)PHYS_TO_VIRT(dcbaa_page);

    // Scratchpad buffer support (required when Max Scratchpad Buffers > 0).
    uint32_t sp_hi = (hcsparams2 >> 27) & 0x1F;
    uint32_t sp_lo = (hcsparams2 >> 21) & 0x1F;
    uint32_t sp_count = (sp_hi << 5) | sp_lo;
    g_scratchpad_count = sp_count;
    g_scratchpad_array_phys = 0;
    if (sp_count > 0) {
        void* sp_array_page = pmm_alloc(1);
        if (!sp_array_page) return -1;
        memzero(PHYS_TO_VIRT(sp_array_page), PAGE_SIZE);
        g_scratchpad_array_phys = (uint64_t)sp_array_page;

        volatile uint64_t* sp_array = (volatile uint64_t*)PHYS_TO_VIRT(sp_array_page);
        for (uint32_t i = 0; i < sp_count; i++) {
            void* sp_buf = pmm_alloc(1);
            if (!sp_buf) return -1;
            memzero(PHYS_TO_VIRT(sp_buf), PAGE_SIZE);
            sp_array[i] = (uint64_t)sp_buf;
        }
        dcbaa[0] = g_scratchpad_array_phys;
    }

    // 2) Allocate command ring page (TRBs). Link TRB at tail points to head.
    void* cmd_page = pmm_alloc(1);
    if (!cmd_page) return -1;
    memzero(PHYS_TO_VIRT(cmd_page), PAGE_SIZE);
    g_cmd_ring_phys = (uint64_t)cmd_page;
    volatile uint32_t* cmd = (volatile uint32_t*)PHYS_TO_VIRT(cmd_page);
    // 256 TRBs per 4KiB. Reserve the last one as Link TRB.
    const uint32_t link_index = 255;
    uint64_t cmd_ring_target = g_cmd_ring_phys & ~0xFULL;
    cmd[link_index * 4 + 0] = (uint32_t)(cmd_ring_target & 0xFFFFFFFFU);
    cmd[link_index * 4 + 1] = (uint32_t)(cmd_ring_target >> 32);
    cmd[link_index * 4 + 2] = 0;
    // Link TRB (type=6) + Toggle Cycle(bit1) + Cycle(bit0=1 at init PCS).
    cmd[link_index * 4 + 3] = (6U << 10) | (1U << 1) | 1U;

    // 3) Allocate one ERST entry and one Event Ring page.
    void* erst_page = pmm_alloc(1);
    void* evt_page = pmm_alloc(1);
    if (!erst_page || !evt_page) return -1;
    memzero(PHYS_TO_VIRT(erst_page), PAGE_SIZE);
    memzero(PHYS_TO_VIRT(evt_page), PAGE_SIZE);
    g_erst_phys = (uint64_t)erst_page;
    g_event_ring_phys = (uint64_t)evt_page;

    volatile uint32_t* erst = (volatile uint32_t*)PHYS_TO_VIRT(erst_page);
    const uint32_t erst_size = 256; // number of TRBs in event ring segment
    erst[0] = (uint32_t)(g_event_ring_phys & 0xFFFFFFFFU);
    erst[1] = (uint32_t)(g_event_ring_phys >> 32);
    erst[2] = erst_size; // Ring Segment Size
    erst[3] = 0;

    // 4) Program DCBAAP, CRCR, runtime interrupter-0 ERST registers.
    mmio_write64(op, 0x30, g_dcbaap_phys);
    // CRCR: dequeue ptr + RCS(bit0)=1
    mmio_write64(op, 0x18, (g_cmd_ring_phys & ~0x3FULL) | 1ULL);
    // CONFIG.MaxSlotsEn
    mmio_write32(op, 0x38, (max_slots > 8) ? 8 : max_slots);

    volatile uint8_t* rt = cap + (g_xhci_rtsoff & ~0x1FU);
    // Interrupter 0 register set starts at runtime + 0x20.
    const uint32_t intr0 = 0x20;
    mmio_write32(rt, intr0 + 0x08, 1); // ERSTSZ
    mmio_write64(rt, intr0 + 0x10, g_erst_phys); // ERSTBA
    // ERDP points to first event TRB.
    mmio_write64(rt, intr0 + 0x18, (g_event_ring_phys & ~0xFULL));
    mmio_write32(rt, intr0 + 0x04, 0); // IMOD
    mmio_write32(rt, intr0 + 0x00, (1U << 1)); // IMAN.IE

    g_xhci_rings_ready = 1;
    g_cmd_enqueue_idx = 0;
    g_cmd_cycle = 1;
    g_evt_dequeue_idx = 0;
    g_evt_cycle = 1;
    g_usb_msc_tag = 1;
    return 0;
}

int usb_is_ready(void) {
    return g_usb_ready;
}

int usb_mass_storage_ready(void) {
    return g_usb_mass_ready;
}

int usb_block_device_ready(void) {
    return g_usb_msc_bot_ready && g_usb_msc_capacity_ok;
}

int usb_read_block(uint32_t lba, void* buf, uint32_t count) {
    if (!buf || count == 0) return -1;
    if (!usb_block_device_ready()) return -2;
    if (g_usb_block_size == 0 || count > ((USB_BULK_BUF_SIZE - USB_BULK_DATA_OFFSET) / g_usb_block_size)) return -3;
    if (lba >= g_usb_block_count) return -4;
    if (lba + count > g_usb_block_count) return -5;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (usb_msc_scsi_read10(lba, buf, count) == 0) return 0;
        (void)usb_msc_recover_bulk_path();
        (void)usb_msc_scsi_test_unit_ready();
    }
    return -6;
}

void usb_init(void) {
    g_usb_ready = 0;
    g_usb_mass_ready = 0;
    g_xhci_rings_ready = 0;
    g_xhci_cmd_ready = 0;
    g_xhci_addr_ready = 0;
    g_xhci_cfg_ready = 0;
    g_usb_desc_ready = 0;
    g_usb_cfg_desc_ready = 0;
    g_usb_msc_if_ready = 0;
    g_usb_msc_bot_ready = 0;
    g_usb_msc_inquiry_ok = 0;
    g_usb_msc_capacity_ok = 0;
    g_usb_vid = 0;
    g_usb_pid = 0;
    g_usb_dev_class = 0;
    g_usb_dev_subclass = 0;
    g_usb_dev_proto = 0;
    g_usb_msc_if_class = 0;
    g_usb_msc_if_subclass = 0;
    g_usb_msc_if_proto = 0;
    g_usb_cfg_value = 0;
    g_usb_msc_if_number = 0;
    g_usb_bulk_out_ep = 0;
    g_usb_bulk_in_ep = 0;
    g_usb_ep0_mps = 64;
    g_usb_bulk_out_mps = 0;
    g_usb_bulk_in_mps = 0;
    g_usb_bulk_out_dci = 0;
    g_usb_bulk_in_dci = 0;
    g_usb_block_count = 0;
    g_usb_block_size = 0;
    g_xhci_slot_id = 0;
    g_xhci_port_id = 0;
    g_xhci_mmio = 0;

    struct pci_device_info xhci;
    if (pci_find_xhci(&xhci) < 0) {
        puts("[usb] no xHCI controller\r\n");
        return;
    }
    pci_enable_mmio_busmaster(&xhci);

    uint64_t mmio_phys = pci_get_bar0_mmio(&xhci);
    if (mmio_phys == 0) {
        puts("[usb] xHCI BAR0 invalid\r\n");
        return;
    }

    // Current kernel maps HHDM only for low 4GiB during boot.
    if (mmio_phys >= 0x100000000ULL) {
        puts("[usb] xHCI BAR0 above 4GiB is not mapped yet\r\n");
        puthex(mmio_phys);
        puts("\r\n");
        return;
    }

    volatile uint8_t* cap = (volatile uint8_t*)PHYS_TO_VIRT(mmio_phys);
    g_cap_regs = cap;
    uint8_t caplen = cap[0];
    uint16_t hciversion = (uint16_t)cap[2] | ((uint16_t)cap[3] << 8);
    uint32_t hcsparams1 = mmio_read32(cap, 0x04);
    uint32_t hcsparams2 = mmio_read32(cap, 0x08);
    uint32_t hccparams1 = mmio_read32(cap, 0x10);
    g_xhci_max_ports = (uint8_t)((hcsparams1 >> 24) & 0xFF);
    g_xhci_dboff = mmio_read32(cap, 0x14);
    g_xhci_rtsoff = mmio_read32(cap, 0x18);
    g_xhci_ctx_size = (hccparams1 & (1U << 2)) ? 64U : 32U;

    // xHCI bring-up: stop -> reset -> setup rings -> run
    volatile uint8_t* op = cap + caplen;
    g_op_regs = op;
    g_db_regs = cap + (g_xhci_dboff & ~0x3U);
    g_rt_regs = cap + (g_xhci_rtsoff & ~0x1FU);
    int cmd_probe = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t usbcmd = mmio_read32(op, 0x00);
        mmio_write32(op, 0x00, usbcmd & ~1U); // RS=0 (stop)
        (void)xhci_wait_bits(op, 0x04, (1U << 0), (1U << 0), 500000); // USBSTS.HCHalted=1

        usbcmd = mmio_read32(op, 0x00);
        mmio_write32(op, 0x00, usbcmd | (1U << 1)); // HCRST=1
        if (xhci_wait_bits(op, 0x00, (1U << 1), 0, 2000000) < 0) {
            puts("[usb] xHCI reset timeout\r\n");
            return;
        }
        // Wait for Controller Not Ready (CNR, USBSTS bit11) to clear.
        if (xhci_wait_bits(op, 0x04, (1U << 11), 0, 2000000) < 0) {
            puts("[usb] xHCI CNR timeout\r\n");
            return;
        }

        if (xhci_setup_rings(cap, op, hcsparams1, hcsparams2) < 0) {
            puts("[usb] xHCI ring setup failed\r\n");
            return;
        }

        // Select 4KiB page size (bit0) for context/data structures.
        mmio_write32(op, 0x08, 1U);

        // Clear pending status bits before Run.
        mmio_write32(op, 0x04, 0xFFFFFFFFU);

        usbcmd = mmio_read32(op, 0x00);
        // Run + INTE (interrupts); events are still polled path for now.
        mmio_write32(op, 0x00, usbcmd | 1U | (1U << 2)); // RS=1, INTE=1
        if (xhci_wait_bits(op, 0x04, (1U << 0), 0, 2000000) < 0) {
            puts("[usb] xHCI run timeout\r\n");
            return;
        }

        cmd_probe = xhci_cmd_noop();
        if (cmd_probe == 0) {
            g_xhci_cmd_ready = 1;
            break;
        }

        if (attempt == 0) {
            puts("[usb] xHCI command probe retry\r\n");
        }
    }

    if (!g_xhci_cmd_ready) {
        puts("[usb] xHCI command ring probe failed code=");
        puthex((uint64_t)(uint32_t)(-cmd_probe));
        puts("\r\n");
    } else {
        uint8_t slot_id = 0;
        int es = xhci_cmd_enable_slot(&slot_id);
        if (es == 0) {
            g_xhci_slot_id = slot_id;
            puts("[usb] Enable Slot OK slot=");
            puthex(slot_id);
            puts("\r\n");

            // Use first connected root-hub port for Address Device.
            for (uint8_t p = 1; p <= g_xhci_max_ports; p++) {
                uint32_t portsc = mmio_read32(op, 0x400 + ((uint32_t)p - 1U) * 0x10);
                if (portsc & 0x1U) {
                    g_xhci_port_id = p;
                    break;
                }
            }
            if (g_xhci_port_id != 0) {
                int ad = xhci_cmd_address_device(g_xhci_slot_id, g_xhci_port_id);
                if (ad == 0) {
                    g_xhci_addr_ready = 1;
                    puts("[usb] Address Device OK port=");
                    puthex(g_xhci_port_id);
                    puts("\r\n");

                    int ce = xhci_cmd_configure_endpoint(g_xhci_slot_id);
                    if (ce == 0) {
                        g_xhci_cfg_ready = 1;
                        puts("[usb] Configure Endpoint OK slot=");
                        puthex(g_xhci_slot_id);
                        puts("\r\n");
                    } else {
                        // For default EP0 path, many controllers allow proceeding
                        // after Address Device even if Configure Endpoint is rejected.
                        g_xhci_cfg_ready = 1;
                        puts("[usb] Configure Endpoint skipped code=");
                        puthex((uint64_t)(uint32_t)(-ce));
                        puts(" (continue with EP0)\r\n");
                    }

                    int gd = xhci_ep0_get_device_descriptor(g_xhci_slot_id);
                    if (gd == 0) {
                        puts("[usb] GET_DESCRIPTOR(Device) OK vid=0x");
                        puthex(g_usb_vid);
                        puts(" pid=0x");
                        puthex(g_usb_pid);
                        puts(" class=0x");
                        puthex(g_usb_dev_class);
                        puts("\r\n");
                    } else {
                        puts("[usb] GET_DESCRIPTOR(Device) failed code=");
                        puthex((uint64_t)(uint32_t)(-gd));
                        puts("\r\n");
                    }

                    int gc = xhci_ep0_get_config_descriptor(g_xhci_slot_id);
                    if (gc == 0) {
                        puts("[usb] GET_DESCRIPTOR(Config) MSC if class=0x");
                        puthex(g_usb_msc_if_class);
                        puts(" sub=0x");
                        puthex(g_usb_msc_if_subclass);
                        puts(" proto=0x");
                        puthex(g_usb_msc_if_proto);
                        puts(" cfg=0x");
                        puthex(g_usb_cfg_value);
                        puts(" if=0x");
                        puthex(g_usb_msc_if_number);
                        puts(" epout=0x");
                        puthex(g_usb_bulk_out_ep);
                        puts(" epin=0x");
                        puthex(g_usb_bulk_in_ep);
                        puts("\r\n");
                    } else {
                        puts("[usb] GET_DESCRIPTOR(Config) failed code=");
                        puthex((uint64_t)(uint32_t)(-gc));
                        puts("\r\n");
                        // QEMU usb-storage fallback: device class is often 0x00
                        // and interface descriptor fetch may be flaky in early stack.
                        if (g_usb_vid == 0x46F4 && g_usb_pid == 0x0001) {
                            g_usb_msc_if_ready = 1;
                            g_usb_cfg_value = 1;
                            g_usb_msc_if_number = 0;
                            g_usb_msc_if_class = 0x08;
                            g_usb_msc_if_subclass = 0x06;
                            g_usb_msc_if_proto = 0x50;
                            g_usb_bulk_out_ep = 0x01;
                            g_usb_bulk_in_ep = 0x82;
                            g_usb_bulk_out_mps = 64;
                            g_usb_bulk_in_mps = 64;
                            g_usb_bulk_out_dci = xhci_dci_from_epaddr(g_usb_bulk_out_ep);
                            g_usb_bulk_in_dci = xhci_dci_from_epaddr(g_usb_bulk_in_ep);
                            puts("[usb] Applying QEMU MSC fallback (46f4:0001)\r\n");
                        }
                    }
                    if (g_usb_msc_if_ready) {
                        int can_continue = 0;
                        uint64_t set_cfg = 0;
                        set_cfg |= 0x00ULL;
                        set_cfg |= (uint64_t)9 << 8; // SET_CONFIGURATION
                        set_cfg |= (uint64_t)g_usb_cfg_value << 16;
                        int sc = xhci_ep0_control_no_data(g_xhci_slot_id, set_cfg);
                        if (sc == 0) {
                            can_continue = 1;
                        } else if (g_usb_vid == 0x46F4 && g_usb_pid == 0x0001) {
                            puts("[usb] SET_CONFIGURATION failed, continue with QEMU fallback\r\n");
                            can_continue = 1;
                        } else {
                            puts("[usb] SET_CONFIGURATION failed code=");
                            puthex((uint64_t)(uint32_t)(-sc));
                            puts("\r\n");
                        }

                        if (can_continue) {
                            int be;
                            if (!g_usb_cfg_desc_ready && g_usb_vid == 0x46F4 && g_usb_pid == 0x0001) {
                                be = usb_try_qemu_bulk_fallback(g_xhci_slot_id);
                            } else {
                                be = xhci_setup_bulk_endpoints(g_xhci_slot_id);
                            }
                            if (be == 0) {
                                int iq = usb_msc_scsi_inquiry();
                                int rc = usb_msc_scsi_read_capacity10();
                                if (iq == 0 && rc == 0) {
                                    g_usb_msc_bot_ready = 1;
                                    puts("[usb] MSC BOT ready blocks=0x");
                                    puthex(g_usb_block_count);
                                    puts(" block_size=0x");
                                    puthex(g_usb_block_size);
                                    puts("\r\n");
                                } else {
                                    puts("[usb] MSC BOT probe failed inquiry=0x");
                                    puthex((uint64_t)(uint32_t)(-iq));
                                    puts(" capacity=0x");
                                    puthex((uint64_t)(uint32_t)(-rc));
                                    puts("\r\n");
                                }
                            } else {
                                puts("[usb] bulk endpoint setup failed code=");
                                    puthex((uint64_t)(uint32_t)(-be));
                                    puts("\r\n");
                            }
                        }
                    }
                } else {
                    puts("[usb] Address Device failed code=");
                    puthex((uint64_t)(uint32_t)(-ad));
                    puts("\r\n");
                }
            }
        } else {
            puts("[usb] Enable Slot failed code=");
            puthex((uint64_t)(uint32_t)(-es));
            puts("\r\n");
        }
    }

    g_xhci_mmio = mmio_phys;
    g_usb_ready = 1;

    puts("[usb] xHCI capability mapped at phys=0x");
    puthex(mmio_phys);
    puts(" caplen=0x");
    puthex(caplen);
    puts(" hciver=0x");
    puthex(hciversion);
    puts(" hcsparams1=0x");
    puthex(hcsparams1);
    puts(" hcsparams2=0x");
    puthex(hcsparams2);
    puts(" dboff=0x");
    puthex(g_xhci_dboff);
    puts(" rtsoff=0x");
    puthex(g_xhci_rtsoff);
    puts(" cmd=");
    puts(g_xhci_cmd_ready ? "ok" : "fail");
    puts(" addr=");
    puts(g_xhci_addr_ready ? "ok" : "fail");
    puts(" cfg=");
    puts(g_xhci_cfg_ready ? "ok" : "fail");
    puts(" desc=");
    puts(g_usb_desc_ready ? "ok" : "fail");
    puts(" cfgd=");
    puts(g_usb_cfg_desc_ready ? "ok" : "fail");
    puts(" mscif=");
    puts(g_usb_msc_if_ready ? "ok" : "fail");
    puts(" slot=");
    puthex(g_xhci_slot_id);
    puts(" port=");
    puthex(g_xhci_port_id);
    puts(" sp=");
    puthex(g_scratchpad_count);
    puts("\r\n");

    // Probe connected ports. Full enumeration/BOT/SCSI is next phase.
    uint32_t connected = 0;
    uint32_t enabled = 0;
    for (uint8_t p = 0; p < g_xhci_max_ports; p++) {
        uint32_t portsc = mmio_read32(op, 0x400 + (uint32_t)p * 0x10);
        if (portsc & 0x1U) connected++;
        if (portsc & 0x2U) enabled++;
    }
    if (connected > 0 && g_xhci_rings_ready && g_xhci_cmd_ready && g_usb_msc_bot_ready) {
        g_usb_mass_ready = 1;
    }
    puts("[usb] ports: max=");
    puthex(g_xhci_max_ports);
    puts(" connected=");
    puthex(connected);
    puts(" enabled=");
    puthex(enabled);
    puts(" rings=");
    puts(g_xhci_rings_ready ? "ready" : "not-ready");
    puts(" cmd=");
    puts(g_xhci_cmd_ready ? "ok" : "fail");
    puts(" addr=");
    puts(g_xhci_addr_ready ? "ok" : "fail");
    puts(" cfg=");
    puts(g_xhci_cfg_ready ? "ok" : "fail");
    puts(" desc=");
    puts(g_usb_desc_ready ? "ok" : "fail");
    puts(" cfgd=");
    puts(g_usb_cfg_desc_ready ? "ok" : "fail");
    puts(" mscif=");
    puts(g_usb_msc_if_ready ? "ok" : "fail");
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    puts(" slot=");
    puthex(g_xhci_slot_id);
    puts(" port=");
    puthex(g_xhci_port_id);
    puts("\r\n");

    if (!g_usb_mass_ready) {
        puts("[usb] Mass Storage transport is not implemented yet\r\n");
    }
}

void usb_dump_status(void) {
    puts("[usb] status: xhci=");
    puts(g_usb_ready ? "ready" : "not-ready");
    puts(" msc=");
    puts(g_usb_mass_ready ? "ready" : "not-ready");
    puts(" rings=");
    puts(g_xhci_rings_ready ? "ready" : "not-ready");
    puts(" cmd=");
    puts(g_xhci_cmd_ready ? "ok" : "fail");
    puts(" addr=");
    puts(g_xhci_addr_ready ? "ok" : "fail");
    puts(" cfg=");
    puts(g_xhci_cfg_ready ? "ok" : "fail");
    puts(" desc=");
    puts(g_usb_desc_ready ? "ok" : "fail");
    puts(" cfgd=");
    puts(g_usb_cfg_desc_ready ? "ok" : "fail");
    puts(" mscif=");
    puts(g_usb_msc_if_ready ? "ok" : "fail");
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    puts(" slot=");
    puthex(g_xhci_slot_id);
    puts(" port=");
    puthex(g_xhci_port_id);
    puts(" mmio=0x");
    puthex(g_xhci_mmio);
    puts(" dcbaa=0x");
    puthex(g_dcbaap_phys);
    puts(" cr=0x");
    puthex(g_cmd_ring_phys);
    puts(" er=0x");
    puthex(g_event_ring_phys);
    puts(" sp=0x");
    puthex(g_scratchpad_array_phys);
    puts(" inctx=0x");
    puthex(g_input_ctx_phys);
    puts(" outctx=0x");
    puthex(g_output_ctx_phys);
    puts(" ep0=0x");
    puthex(g_ep0_ring_phys);
    puts(" vid=0x");
    puthex(g_usb_vid);
    puts(" pid=0x");
    puthex(g_usb_pid);
    puts(" cls=0x");
    puthex(g_usb_dev_class);
    puts(" sub=0x");
    puthex(g_usb_dev_subclass);
    puts(" pr=0x");
    puthex(g_usb_dev_proto);
    puts(" ifc=0x");
    puthex(g_usb_msc_if_class);
    puts(" ifs=0x");
    puthex(g_usb_msc_if_subclass);
    puts(" ifp=0x");
    puthex(g_usb_msc_if_proto);
    puts(" cfgv=0x");
    puthex(g_usb_cfg_value);
    puts(" ifn=0x");
    puthex(g_usb_msc_if_number);
    puts(" epout=0x");
    puthex(g_usb_bulk_out_ep);
    puts(" epin=0x");
    puthex(g_usb_bulk_in_ep);
    puts(" dciout=0x");
    puthex(g_usb_bulk_out_dci);
    puts(" dciin=0x");
    puthex(g_usb_bulk_in_dci);
    puts(" bot=");
    puts(g_usb_msc_bot_ready ? "ok" : "fail");
    puts(" iq=");
    puts(g_usb_msc_inquiry_ok ? "ok" : "fail");
    puts(" cap=");
    puts(g_usb_msc_capacity_ok ? "ok" : "fail");
    puts(" blocks=0x");
    puthex(g_usb_block_count);
    puts(" blksz=0x");
    puthex(g_usb_block_size);
    puts("\r\n");
}
