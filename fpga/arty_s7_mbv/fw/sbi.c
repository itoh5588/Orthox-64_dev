/*
 * Minimal SBI for Orthox on the Arty S7-50 MicroBlaze V SoC.
 *
 * Provides BASE, TIME (backed by the Sstc stimecmp CSR) and the legacy
 * console calls. Everything else returns SBI_ERR_NOT_SUPPORTED, which
 * Orthox's SMP probe treats as "no other harts".
 *
 * External interrupts: the AXI INTC drives the M-mode external interrupt
 * (MEIP), which cannot be delegated. The firmware masks MEIE, raises SEIP
 * and returns; the kernel services the INTC and then calls the Orthox
 * EXTIRQ "complete" function, which drops SEIP and unmasks MEIE again.
 *
 * Kernel: loaded over JTAG (fw/run.sh), or else read from the SD card, where
 * fw/install-kernel.sh has put the ELF file at KIMG_LBA (one header sector,
 * then the file). The firmware copies the PT_LOAD segments, zeroing the
 * rest of each up to p_memsz.
 */
#include <stdint.h>

#define KERNEL_ENTRY 0x80200000UL
#define UART_BASE    0x40600000UL
#define SD_BASE      0x40800000UL

#define KIMG_LBA     0x200000UL         /* 1 GiB into the card */
#define KIMG_STAGE   0x8F000000UL       /* ELF file buffer, top 16 MiB of DDR */
#define KIMG_MAX     0x01000000UL
#define KIMG_MAGIC   0x31304E52454B584FUL  /* "OXKERN01" */

#define SBI_EXT_LEGACY_SET_TIMER 0x00
#define SBI_EXT_LEGACY_PUTCHAR   0x01
#define SBI_EXT_LEGACY_GETCHAR   0x02
#define SBI_EXT_BASE             0x10
#define SBI_EXT_TIME             0x54494D45
#define SBI_EXT_ORTHOX_EXTIRQ    0x0A4F5258  /* firmware-specific range */

#define SBI_SUCCESS            0
#define SBI_ERR_NOT_SUPPORTED (-2)

#define CSR_MENVCFG   0x30A
#define CSR_STIMECMP  0x14D
#define MENVCFG_STCE  (1UL << 63)
#define MIP_SEIP      (1UL << 9)
#define MIE_MEIE      (1UL << 11)
#define MCAUSE_MEXT   ((1UL << 63) | 11)

#define MSTATUS_MPP_S (1UL << 11)
#define MSTATUS_MPP   (3UL << 11)
#define MSTATUS_MPIE  (1UL << 7)
#define MSTATUS_FS_INITIAL (1UL << 13)

#define csr_read(csr) ({ uint64_t v_; __asm__ volatile("csrr %0, " #csr : "=r"(v_)); v_; })
#define csr_write(csr, v) __asm__ volatile("csrw " #csr ", %0" :: "r"((uint64_t)(v)))

struct fw_frame {
    uint64_t x[32];
};

extern const char fw_dtb[], fw_dtb_end[];

/* Kernel .bss range, written by the JTAG loader (run.tcl) after loading the
 * ELF; the loader does not clear NOBITS sections. It also tells us a kernel
 * is there: when the firmware starts from the bitstream's BRAM contents
 * (power-on, no JTAG) it is still 0 and DDR holds no kernel. */
volatile uint64_t fw_zero_start __attribute__((section(".data"))) = 0;
volatile uint64_t fw_zero_end __attribute__((section(".data"))) = 0;

/* Set by fw/install-kernel.sh: an ELF file at KIMG_STAGE to write to the SD
 * card before booting. */
volatile uint64_t fw_install_size __attribute__((section(".data"))) = 0;

static volatile uint32_t* uart_reg(uint32_t off) {
    return (volatile uint32_t*)(UART_BASE + off);
}

static void putc(char c) {
    while (*uart_reg(0x8) & 0x08) {
    }
    *uart_reg(0x4) = (uint8_t)c;
}

static int getc_nonblock(void) {
    if ((*uart_reg(0x8) & 0x01) == 0) return -1;
    return (int)(*uart_reg(0x0) & 0xff);
}

static void puts(const char* s) {
    while (*s) {
        if (*s == '\n') putc('\r');
        putc(*s++);
    }
}

static void puthex(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) putc(hex[(v >> shift) & 0xf]);
}

static void putkv(const char* key, uint64_t v) {
    puts(key);
    puts("0x");
    puthex(v);
    puts("\n");
}

/* SD card (fpga/arty_s7_mbv/rtl/sd_axi.vhd), polled. */
#define SD_OP_INIT  1
#define SD_OP_READ  4
#define SD_OP_WRITE 6

static volatile uint32_t* sd_reg(uint32_t off) {
    return (volatile uint32_t*)(SD_BASE + off);
}

/* Returns 0, or the status word (error code in bits 15:8) on failure. */
static uint32_t sd_run(uint32_t op) {
    uint64_t t0 = csr_read(time);
    uint32_t st;

    *sd_reg(0x0) = op;
    while ((st = *sd_reg(0x0)) & 1) {
        if (csr_read(time) - t0 > 2UL * 50000000UL) return st | 0xff00;
    }
    return (st & 0xff00) ? st : 0;
}

static uint32_t sd_read(uint64_t lba, void* buf) {
    uint32_t* w = buf;
    uint32_t st;

    *sd_reg(0x4) = (uint32_t)lba;
    if ((st = sd_run(SD_OP_READ)) != 0) return st;
    for (int i = 0; i < 128; i++) w[i] = *sd_reg(0x400 + 4 * i);
    return 0;
}

static uint32_t sd_write(uint64_t lba, const void* buf) {
    const uint32_t* w = buf;

    for (int i = 0; i < 128; i++) *sd_reg(0x600 + 4 * i) = w[i];
    *sd_reg(0x4) = (uint32_t)lba;
    return sd_run(SD_OP_WRITE);
}

static uint32_t sd_init(void) {
    uint32_t st = 0;

    if ((*sd_reg(0x0) & 0x10) == 0) return 0xfe00;     /* no card */
    for (int i = 0; i < 3; i++) {
        if ((st = sd_run(SD_OP_INIT)) == 0) return 0;
    }
    return st;
}

struct kimg_hdr {
    uint64_t magic;
    uint64_t size;
    uint64_t sum;
};

static uint64_t kimg_sum(const uint8_t* p, uint64_t n) {
    uint64_t sum = 0;
    for (uint64_t i = 0; i < n; i++) sum = sum * 31 + p[i];
    return sum;
}

static int install_kernel(void) {
    static uint64_t sec[64];
    struct kimg_hdr* h = (struct kimg_hdr*)sec;
    const uint8_t* p = (const uint8_t*)KIMG_STAGE;
    uint64_t n = fw_install_size, nsec = (n + 511) / 512;
    uint32_t st;

    puts("fw: writing the kernel to SD\n");
    if (n > KIMG_MAX) return -1;
    if ((st = sd_init()) != 0) {
        putkv("  sd init: ", st);
        return -1;
    }
    for (int i = 0; i < 64; i++) sec[i] = 0;
    h->magic = KIMG_MAGIC;
    h->size = n;
    h->sum = kimg_sum(p, n);
    for (uint64_t i = 0; i < nsec; i++) {
        if ((st = sd_write(KIMG_LBA + 1 + i, p + 512 * i)) != 0) {
            putkv("  sd write: ", st);
            return -1;
        }
    }
    /* header last, so a partial write leaves no valid image */
    if ((st = sd_write(KIMG_LBA, sec)) != 0) {
        putkv("  sd write: ", st);
        return -1;
    }
    putkv("  bytes  : ", n);
    putkv("  sum    : ", h->sum);
    return 0;
}

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

/* Read the kernel ELF from SD and lay it out in DDR. Returns its entry or 0. */
static uint64_t load_kernel(void) {
    static uint64_t sec[64];
    struct kimg_hdr* h = (struct kimg_hdr*)sec;
    uint8_t* p = (uint8_t*)KIMG_STAGE;
    const struct elf64_ehdr* eh = (const struct elf64_ehdr*)p;
    uint64_t n, sum, t0;
    uint32_t st;

    puts("fw: loading the kernel from SD\n");
    if ((st = sd_init()) != 0) {
        putkv("  sd init: ", st);
        return 0;
    }
    if ((st = sd_read(KIMG_LBA, sec)) != 0) {
        putkv("  sd read: ", st);
        return 0;
    }
    if (h->magic != KIMG_MAGIC || h->size == 0 || h->size > KIMG_MAX) {
        puts("  no kernel image on the card\n");
        return 0;
    }
    n = h->size;
    sum = h->sum;
    t0 = csr_read(time);
    for (uint64_t i = 0; i < (n + 511) / 512; i++) {
        if ((st = sd_read(KIMG_LBA + 1 + i, p + 512 * i)) != 0) {
            putkv("  sd read: ", st);
            return 0;
        }
    }
    putkv("  bytes  : ", n);
    putkv("  ms     : ", (csr_read(time) - t0) / 50000);
    if (kimg_sum(p, n) != sum) {
        puts("  checksum mismatch\n");
        return 0;
    }
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->phentsize != sizeof(struct elf64_phdr)) {
        puts("  not an ELF64 file\n");
        return 0;
    }
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr* ph =
            (const struct elf64_phdr*)(p + eh->phoff + (uint64_t)i * eh->phentsize);
        uint8_t* dst = (uint8_t*)ph->paddr;
        if (ph->type != 1) continue;            /* PT_LOAD */
        if (ph->offset + ph->filesz > n) return 0;
        for (uint64_t j = 0; j < ph->filesz; j++) dst[j] = p[ph->offset + j];
        for (uint64_t j = ph->filesz; j < ph->memsz;) {     /* .bss is MiBs */
            if (((uint64_t)(dst + j) & 7) == 0 && ph->memsz - j >= 8) {
                *(uint64_t*)(dst + j) = 0;
                j += 8;
            } else {
                dst[j++] = 0;
            }
        }
    }
    __asm__ volatile(".word 0x0000100f" ::: "memory");     /* fence.i */
    return eh->entry;
}

static void sbi_call(struct fw_frame* f) {
    uint64_t ext = f->x[17], fid = f->x[16], a0 = f->x[10];
    long err = SBI_SUCCESS;
    long val = 0;

    switch (ext) {
    case SBI_EXT_LEGACY_SET_TIMER:
        csr_write(0x14D, a0);
        f->x[10] = 0;
        return;
    case SBI_EXT_LEGACY_PUTCHAR:
        putc((char)a0);
        f->x[10] = 0;
        return;
    case SBI_EXT_LEGACY_GETCHAR:
        f->x[10] = (uint64_t)(long)getc_nonblock();
        return;
    case SBI_EXT_TIME:
        if (fid == 0) csr_write(0x14D, a0);
        else err = SBI_ERR_NOT_SUPPORTED;
        break;
    case SBI_EXT_ORTHOX_EXTIRQ:
        if (fid == 0) {                         /* complete */
            __asm__ volatile("csrc mip, %0" :: "r"(MIP_SEIP));
            __asm__ volatile("csrs mie, %0" :: "r"(MIE_MEIE));
        } else {
            err = SBI_ERR_NOT_SUPPORTED;
        }
        break;
    case SBI_EXT_BASE:
        switch (fid) {
        case 0: val = 0x01000000; break;            /* spec v1.0 */
        case 1: val = 0x0BAD; break;                /* impl id: not registered */
        case 2: val = 1; break;
        case 3:
            val = (a0 == SBI_EXT_BASE || a0 == SBI_EXT_TIME || a0 == SBI_EXT_ORTHOX_EXTIRQ ||
                   a0 == SBI_EXT_LEGACY_SET_TIMER || a0 == SBI_EXT_LEGACY_PUTCHAR ||
                   a0 == SBI_EXT_LEGACY_GETCHAR);
            break;
        case 4: val = (long)csr_read(mvendorid); break;
        case 5: val = (long)csr_read(marchid); break;
        case 6: val = (long)csr_read(mimpid); break;
        default: err = SBI_ERR_NOT_SUPPORTED; break;
        }
        break;
    default:
        err = SBI_ERR_NOT_SUPPORTED;
        break;
    }
    f->x[10] = (uint64_t)err;
    f->x[11] = (uint64_t)val;
}

void fw_trap_handler(struct fw_frame* f) {
    uint64_t cause = csr_read(mcause);
    uint64_t epc = csr_read(mepc);

    if (cause == MCAUSE_MEXT) {         /* AXI INTC: hand it to S-mode */
        __asm__ volatile("csrc mie, %0" :: "r"(MIE_MEIE));
        __asm__ volatile("csrs mip, %0" :: "r"(MIP_SEIP));
        return;
    }
    if (cause == 9) {                   /* ecall from S-mode */
        sbi_call(f);
        csr_write(mepc, epc + 4);
        return;
    }
    puts("\nfw: unexpected M-mode trap\n");
    putkv("  mcause : ", cause);
    putkv("  mepc   : ", epc);
    putkv("  mtval  : ", csr_read(mtval));
    putkv("  mstatus: ", csr_read(mstatus));
    putkv("  ra     : ", f->x[1]);
    putkv("  sp     : ", f->x[2]);
    for (;;) {
    }
}

void fw_main(void) {
    uint64_t t0, t1, mstatus, entry = KERNEL_ENTRY;

    puts("\nfw: Orthox MicroBlaze V firmware\n");
    putkv("  misa   : ", csr_read(misa));
    putkv("  mimpid : ", csr_read(mimpid));
    t0 = csr_read(time);
    t1 = csr_read(time);
    putkv("  time   : ", t0);
    putkv("  time+  : ", t1);

    /* Sstc: let S-mode own stimecmp, and start with no timer pending.
     * On MicroBlaze V (2025.2, C_USE_SSTC=1) menvcfg.STCE is hardwired to 0
     * (writing all ones reads back 0x71: only FIOM and CBIE/CBCFE stick),
     * yet Sstc acts as enabled: stimecmp raises STIP and S-mode can read
     * and write it without a trap (checked on the board, 2026-10-11).
     * Orthox arms the timer through SBI TIME, so either way works. */
    csr_write(0x30A, csr_read(0x30A) | MENVCFG_STCE);
    csr_write(0x14D, ~0UL);
    putkv("  menvcfg: ", csr_read(0x30A));

    /* cycle/time/instret visible to S and U. */
    csr_write(mcounteren, 7);
    csr_write(scounteren, 7);

    /* Delegate all S/U exceptions except ecall-from-S (9), plus all S interrupts. */
    csr_write(medeleg, 0xB1FF);
    csr_write(mideleg, 0x222);
    csr_write(mie, MIE_MEIE);
    putkv("  medeleg: ", csr_read(medeleg));
    putkv("  mideleg: ", csr_read(mideleg));
    putkv("  dtb    : ", (uint64_t)(uintptr_t)fw_dtb);
    if (fw_install_size != 0 && install_kernel() != 0) {
        puts("fw: kernel install failed\n");
        for (;;) {
        }
    }
    if (fw_zero_end == 0) {             /* nothing over JTAG: try the SD card */
        entry = load_kernel();
        if (entry == 0) {
            puts("fw: no kernel; load one over JTAG (fw/run.sh) or install one (fw/install-kernel.sh)\n");
            for (;;) {
            }
        }
    } else if (fw_zero_end > fw_zero_start) {
        putkv("  bss    : ", fw_zero_start);
        for (uint64_t p = fw_zero_start; p < fw_zero_end; p += 8) *(volatile uint64_t*)p = 0;
    }
    putkv("  kernel : ", entry);
    puts("fw: entering S-mode\n");
    while ((*uart_reg(0x8) & 0x04) == 0) {      /* TX FIFO empty: the kernel resets it */
    }

    mstatus = csr_read(mstatus);
    mstatus &= ~(MSTATUS_MPP | MSTATUS_MPIE);
    mstatus |= MSTATUS_MPP_S | MSTATUS_FS_INITIAL;
    csr_write(mstatus, mstatus);
    csr_write(satp, 0);
    csr_write(mepc, entry);
    {
        register uint64_t a0 __asm__("a0") = csr_read(mhartid);
        register uint64_t a1 __asm__("a1") = (uint64_t)(uintptr_t)fw_dtb;
        __asm__ volatile("mret" :: "r"(a0), "r"(a1));
    }
    for (;;) {
    }
}
