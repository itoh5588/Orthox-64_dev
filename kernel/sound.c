#include <stdint.h>
#include "sound.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

void puts(const char* s);
void puthex(uint64_t v);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

enum sound_backend_kind {
    SOUND_BACKEND_NONE = 0,
    SOUND_BACKEND_SB16 = 1,
    SOUND_BACKEND_AC97 = 2,
};

static int g_sound_initialized = 0;
static enum sound_backend_kind g_sound_pcm_backend = SOUND_BACKEND_NONE;

#define AC97_NAM_RESET          0x00
#define AC97_NAM_MASTER_VOL     0x02
#define AC97_NAM_PCM_OUT_VOL    0x18
#define AC97_NAM_EXT_CAPS       0x28
#define AC97_NAM_EXT_CTRL       0x2A
#define AC97_NAM_PCM_FRONT_RATE 0x2C

#define AC97_NABM_PO_BDBAR      0x10
#define AC97_NABM_PO_CIV        0x14
#define AC97_NABM_PO_LVI        0x15
#define AC97_NABM_PO_SR         0x16
#define AC97_NABM_PO_PICB       0x18
#define AC97_NABM_PO_CR         0x1B
#define AC97_NABM_GLOB_CNT      0x2C

#define AC97_SR_LVBCI           0x0004
#define AC97_SR_DCH             0x0008
#define AC97_SR_CELV            0x0010
#define AC97_SR_W1C_MASK        (AC97_SR_LVBCI | AC97_SR_DCH | AC97_SR_CELV)

#define AC97_CR_RPBM            0x01
#define AC97_CR_RST             0x02

#define AC97_BDL_ENTRIES        32U
#define AC97_FRAG_BYTES         4096U

struct ac97_bdl_entry {
    uint32_t addr;
    uint16_t count;
    uint16_t flags;
} __attribute__((packed));

struct ac97_state {
    int present;
    int initialized;
    int failed;
    struct pci_device_info dev;
    uint16_t nam;
    uint16_t nabm;
    struct ac97_bdl_entry* bdl;
    uint32_t bdl_phys;
    uint8_t* buf;
    uint32_t buf_phys;
    uint32_t frag_bytes;
    uint32_t frag_frames;
    int tail;
    int started;
};

static struct ac97_state g_ac97;

static void puthex8(uint8_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char s[3];
    s[0] = hex[(v >> 4) & 0xF];
    s[1] = hex[v & 0xF];
    s[2] = '\0';
    puts(s);
}

static void sound_log_pci_bdf(const struct pci_device_info* dev) {
    if (!dev) return;
    puthex8(dev->bus);
    puts(":");
    puthex8(dev->device);
    puts(".");
    puthex8(dev->function);
}

static inline uint8_t ac97_po_civ(void) {
    return inb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CIV));
}

static inline void ac97_po_set_lvi(uint8_t v) {
    outb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_LVI), v);
}

static inline void ac97_po_run(void) {
    outb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CR), AC97_CR_RPBM);
}

static inline uint8_t ac97_ring_free32(uint8_t civ, int tail) {
    if (tail < 0) {
        return 32;
    }
    int free = (int)civ - tail - 1;
    while (free < 0) free += 32;
    return (uint8_t)free;
}

static void ac97_stop_clear(void) {
    if (!g_ac97.initialized) return;

    outb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CR), 0);
    outb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CR), AC97_CR_RST);
    for (int i = 0; i < 10000; i++) {
        if ((inb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CR)) & AC97_CR_RST) == 0) break;
    }
    outw((uint16_t)(g_ac97.nabm + AC97_NABM_PO_SR), AC97_SR_W1C_MASK);
    outl((uint16_t)(g_ac97.nabm + AC97_NABM_PO_BDBAR), g_ac97.bdl_phys);
    g_ac97.tail = -1;
    g_ac97.started = 0;
}

static int ac97_hw_init(void) {
    if (!g_ac97.present) return -1;
    if (g_ac97.initialized) return 0;
    if (g_ac97.failed) return -1;

    g_ac97.nam = pci_get_bar_iobase(&g_ac97.dev, 0);
    g_ac97.nabm = pci_get_bar_iobase(&g_ac97.dev, 1);
    if (g_ac97.nam == 0 || g_ac97.nabm == 0) {
        puts("[ac97] missing I/O BARs\r\n");
        g_ac97.failed = 1;
        return -1;
    }

    pci_enable_io_busmaster(&g_ac97.dev);

    outl((uint16_t)(g_ac97.nabm + AC97_NABM_GLOB_CNT), 0x00000002U);
    outw((uint16_t)(g_ac97.nam + AC97_NAM_RESET), 0x0000);
    for (int i = 0; i < 10000; i++) io_wait();
    outw((uint16_t)(g_ac97.nam + AC97_NAM_MASTER_VOL), 0x0000);
    outw((uint16_t)(g_ac97.nam + AC97_NAM_PCM_OUT_VOL), 0x0000);

    uint16_t caps = inw((uint16_t)(g_ac97.nam + AC97_NAM_EXT_CAPS));
    if (caps & 0x0001) {
        uint16_t ec = inw((uint16_t)(g_ac97.nam + AC97_NAM_EXT_CTRL));
        outw((uint16_t)(g_ac97.nam + AC97_NAM_EXT_CTRL), (uint16_t)(ec | 0x0001));
        outw((uint16_t)(g_ac97.nam + AC97_NAM_PCM_FRONT_RATE), 44100);
    }

    void* bdl_phys = pmm_alloc(1);
    void* buf_phys = pmm_alloc(AC97_BDL_ENTRIES);
    if (!bdl_phys || !buf_phys) {
        puts("[ac97] DMA allocation failed\r\n");
        g_ac97.failed = 1;
        return -1;
    }

    g_ac97.bdl = (struct ac97_bdl_entry*)PHYS_TO_VIRT(bdl_phys);
    g_ac97.bdl_phys = (uint32_t)(uint64_t)bdl_phys;
    g_ac97.buf = (uint8_t*)PHYS_TO_VIRT(buf_phys);
    g_ac97.buf_phys = (uint32_t)(uint64_t)buf_phys;
    g_ac97.frag_bytes = AC97_FRAG_BYTES;
    g_ac97.frag_frames = AC97_FRAG_BYTES / 4U;
    g_ac97.tail = -1;
    g_ac97.started = 0;

    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        ((uint8_t*)g_ac97.bdl)[i] = 0;
    }
    for (uint32_t i = 0; i < AC97_BDL_ENTRIES * AC97_FRAG_BYTES; i++) {
        g_ac97.buf[i] = 0;
    }
    for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {
        g_ac97.bdl[i].addr = g_ac97.buf_phys + i * AC97_FRAG_BYTES;
        g_ac97.bdl[i].count = (uint16_t)(g_ac97.frag_frames * 2U);
        g_ac97.bdl[i].flags = 0;
    }

    outb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CR), AC97_CR_RST);
    for (int i = 0; i < 10000; i++) {
        if ((inb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_CR)) & AC97_CR_RST) == 0) break;
    }
    outw((uint16_t)(g_ac97.nabm + AC97_NABM_PO_SR), AC97_SR_W1C_MASK);
    outl((uint16_t)(g_ac97.nabm + AC97_NABM_PO_BDBAR), g_ac97.bdl_phys);
    outb((uint16_t)(g_ac97.nabm + AC97_NABM_PO_LVI), 0);

    g_ac97.initialized = 1;
    puts("[ac97] initialized\r\n");
    return 0;
}

static int ac97_play_pcm_u8(const uint8_t* data, uint32_t len) {
    if (ac97_hw_init() != 0) return -1;
    if (!data || len == 0) return 0;

    uint16_t sr = inw((uint16_t)(g_ac97.nabm + AC97_NABM_PO_SR));
    if (sr & (AC97_SR_DCH | AC97_SR_CELV)) {
        outw((uint16_t)(g_ac97.nabm + AC97_NABM_PO_SR), AC97_SR_W1C_MASK);
        if (g_ac97.started) {
            ac97_po_run();
        }
    }

    uint8_t free = ac97_ring_free32(ac97_po_civ(), g_ac97.tail);
    if (free == 0) {
        return 0;
    }

    uint32_t done = 0;
    while (free && done < len) {
        uint8_t next = (uint8_t)((g_ac97.tail + 1) & 31);
        uint8_t* dst = g_ac97.buf + (uint32_t)next * g_ac97.frag_bytes;
        uint32_t chunk_frames = g_ac97.frag_frames;
        if (chunk_frames > len - done) {
            chunk_frames = len - done;
        }

        for (uint32_t i = 0; i < chunk_frames; i++) {
            int16_t s = (int16_t)(((int)data[done + i] - 128) << 8);
            uint32_t off = i * 4U;
            dst[off + 0] = (uint8_t)(s & 0xFF);
            dst[off + 1] = (uint8_t)((s >> 8) & 0xFF);
            dst[off + 2] = (uint8_t)(s & 0xFF);
            dst[off + 3] = (uint8_t)((s >> 8) & 0xFF);
        }
        for (uint32_t off = chunk_frames * 4U; off < g_ac97.frag_bytes; off++) {
            dst[off] = 0;
        }

        g_ac97.tail = next;
        ac97_po_set_lvi((uint8_t)g_ac97.tail);
        if (!g_ac97.started) {
            ac97_po_run();
            g_ac97.started = 1;
        }

        done += chunk_frames;
        free--;
    }

    return (int)done;
}

void sound_init(void) {
    struct pci_device_info ac97;
    if (g_sound_initialized) return;
    g_sound_initialized = 1;

    if (pci_find_device(&ac97, -1, -1, 0x04, 0x01, -1) == 0) {
        g_ac97.present = 1;
        g_ac97.dev = ac97;
        g_sound_pcm_backend = SOUND_BACKEND_AC97;

        puts("[sound] AC97 detected at ");
        sound_log_pci_bdf(&g_ac97.dev);
        puts(" irq=0x");
        puthex((uint64_t)g_ac97.dev.irq_line);
        puts(" nam=0x");
        puthex((uint64_t)pci_get_bar_iobase(&g_ac97.dev, 0));
        puts(" nabm=0x");
        puthex((uint64_t)pci_get_bar_iobase(&g_ac97.dev, 1));
        puts("\r\n");
        return;
    }

    puts("[sound] AC97 not found, using legacy SB16 path\r\n");
    g_sound_pcm_backend = SOUND_BACKEND_SB16;
}

static int sb16_initialized = 0;
static int sb16_failed = 0;
static uint8_t* sb16_dma_buf = 0;
static uint32_t sb16_dma_phys = 0;
static int sb16_error_reported = 0;

static void sb16_report_once(const char* msg) {
    if (sb16_error_reported) return;
    sb16_error_reported = 1;
    puts("[sb16] ");
    puts(msg);
    puts("\r\n");
}

static int sb16_wait_write_ready(void) {
    // DSP write-buffer status polling (legacy SB16 I/O protocol):
    // bit7=1 means busy, bit7=0 means command/data can be written.
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x22C) & 0x80) == 0) return 0;
    }
    return -1;
}

static int sb16_write(uint8_t v) {
    if (sb16_wait_write_ready() != 0) return -1;
    outb(0x22C, v);
    return 0;
}

static int sb16_prepare_dma_buffer(void) {
    if (sb16_dma_buf) return 0;

    void* phys = pmm_get_isa_dma_page();
    if (!phys) {
        sb16_report_once("no ISA DMA page reserved below 16MiB");
        return -1;
    }

    uint64_t p = (uint64_t)phys;
    sb16_dma_phys = (uint32_t)p;
    sb16_dma_buf = (uint8_t*)PHYS_TO_VIRT(phys);
    if (!sb16_dma_buf) {
        sb16_report_once("failed to map ISA DMA page");
        return -1;
    }

    if ((p & 0xFFFFULL) > (0x10000ULL - 4096ULL)) {
        sb16_report_once("reserved ISA DMA page crosses 64KiB boundary");
        return -1;
    }

    return 0;
}

static void dma_ch1_program(uint32_t phys, uint16_t len) {
    uint16_t count = (uint16_t)(len - 1);

    // mask channel 1
    outb(0x0A, 0x05);
    // clear byte pointer flip-flop
    outb(0x0C, 0x00);
    // base address (channel 1 -> port 0x02)
    outb(0x02, (uint8_t)(phys & 0xFF));
    outb(0x02, (uint8_t)((phys >> 8) & 0xFF));
    // page register for channel 1
    outb(0x83, (uint8_t)((phys >> 16) & 0xFF));
    // count (channel 1 -> port 0x03)
    outb(0x03, (uint8_t)(count & 0xFF));
    outb(0x03, (uint8_t)((count >> 8) & 0xFF));
    // single mode, address increment, memory->device(read), channel 1
    outb(0x0B, 0x49);
    // unmask channel 1
    outb(0x0A, 0x01);
}

static int sb16_init(void) {
    if (sb16_initialized) return 0;
    if (sb16_failed) return -1;

    // Standard SB16 DSP reset handshake:
    // write 1->0 to reset port, then wait for 0xAA from read data port.
    outb(0x226, 1);
    for (int i = 0; i < 64; i++) io_wait();
    outb(0x226, 0);

    // Wait for 0xAA acknowledge
    int ok = 0;
    for (int i = 0; i < 100000; i++) {
        if (inb(0x22E) & 0x80) {
            if (inb(0x22A) == 0xAA) {
                ok = 1;
            }
            break;
        }
    }

    if (!ok) {
        sb16_failed = 1;
        sb16_report_once("DSP reset handshake failed");
        return -1;
    }

    // Enable DSP speaker output.
    if (sb16_write(0xD1) != 0) {
        sb16_failed = 1;
        sb16_report_once("failed to enable DSP speaker");
        return -1;
    }

    sb16_initialized = 1;
    puts("[sb16] initialized\n");
    return 0;
}

void sound_beep_start(uint32_t freq_hz) {
    if (freq_hz < 20) freq_hz = 20;
    if (freq_hz > 20000) freq_hz = 20000;

    uint16_t divisor = (uint16_t)(1193182U / freq_hz);
    if (divisor == 0) divisor = 1;

    // PIT ch2, lobyte/hibyte, square wave
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t val = inb(0x61);
    if ((val & 0x03) != 0x03) {
        outb(0x61, val | 0x03);
    }
}

void sound_beep_stop(void) {
    uint8_t val = inb(0x61) & (uint8_t)~0x03;
    outb(0x61, val);
    if (g_sound_pcm_backend == SOUND_BACKEND_AC97) {
        ac97_stop_clear();
    }
}

int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate) {
    if (!g_sound_initialized) {
        sound_init();
    }
    if (!data || len == 0) return 0;
    if (sample_rate < 4000) sample_rate = 4000;
    if (sample_rate > 44100) sample_rate = 44100;

    if (g_sound_pcm_backend == SOUND_BACKEND_AC97) {
        int ret = ac97_play_pcm_u8(data, len);
        if (ret >= 0) return ret;
    }

    if (sb16_init() != 0) return -1;
    if (sb16_prepare_dma_buffer() != 0) return -1;

    if (len > 4096) len = 4096;
    for (uint32_t i = 0; i < len; i++) {
        sb16_dma_buf[i] = data[i];
    }

    uint32_t tc = 256 - (1000000U / sample_rate);
    if (tc > 255) tc = 255;

    // 0x40: set DSP time constant (8-bit sample clock).
    if (sb16_write(0x40) != 0) {
        sb16_report_once("failed to write time constant command");
        return -1;
    }
    if (sb16_write((uint8_t)tc) != 0) {
        sb16_report_once("failed to write time constant value");
        return -1;
    }

    dma_ch1_program(sb16_dma_phys, (uint16_t)len);

    // 0x14: 8-bit DMA DAC (single-cycle)
    if (sb16_write(0x14) != 0) {
        sb16_report_once("failed to start DMA DAC command");
        return -1;
    }
    if (sb16_write((uint8_t)((len - 1) & 0xFF)) != 0) {
        sb16_report_once("failed to write DMA length low");
        return -1;
    }
    if (sb16_write((uint8_t)(((len - 1) >> 8) & 0xFF)) != 0) {
        sb16_report_once("failed to write DMA length high");
        return -1;
    }

    return (int)len;
}
