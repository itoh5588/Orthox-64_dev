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
 */
#include <stdint.h>

#define KERNEL_ENTRY 0x80200000UL
#define UART_BASE    0x40600000UL

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
 * ELF; the loader does not clear NOBITS sections. 0..0 means nothing to do. */
volatile uint64_t fw_zero_start __attribute__((section(".data"))) = 0;
volatile uint64_t fw_zero_end __attribute__((section(".data"))) = 0;

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
    uint64_t t0, t1, mstatus;

    puts("\nfw: Orthox MicroBlaze V firmware\n");
    putkv("  misa   : ", csr_read(misa));
    putkv("  mimpid : ", csr_read(mimpid));
    t0 = csr_read(time);
    t1 = csr_read(time);
    putkv("  time   : ", t0);
    putkv("  time+  : ", t1);

    /* Sstc: let S-mode own stimecmp, and start with no timer pending. */
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
    putkv("  kernel : ", KERNEL_ENTRY);
    if (fw_zero_end > fw_zero_start) {
        putkv("  bss    : ", fw_zero_start);
        for (uint64_t p = fw_zero_start; p < fw_zero_end; p += 8) *(volatile uint64_t*)p = 0;
    }
    puts("fw: entering S-mode\n");

    mstatus = csr_read(mstatus);
    mstatus &= ~(MSTATUS_MPP | MSTATUS_MPIE);
    mstatus |= MSTATUS_MPP_S | MSTATUS_FS_INITIAL;
    csr_write(mstatus, mstatus);
    csr_write(satp, 0);
    csr_write(mepc, KERNEL_ENTRY);
    {
        register uint64_t a0 __asm__("a0") = csr_read(mhartid);
        register uint64_t a1 __asm__("a1") = (uint64_t)(uintptr_t)fw_dtb;
        __asm__ volatile("mret" :: "r"(a0), "r"(a1));
    }
    for (;;) {
    }
}
