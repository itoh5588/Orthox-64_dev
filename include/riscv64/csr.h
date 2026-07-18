#ifndef ORTHOX_ARCH_RISCV64_CSR_H
#define ORTHOX_ARCH_RISCV64_CSR_H

#include <stdint.h>

#define RISCV64_SSTATUS_SIE   (1UL << 1)
#define RISCV64_SSTATUS_SPIE  (1UL << 5)
#define RISCV64_SSTATUS_SPP   (1UL << 8)
#define RISCV64_SSTATUS_SUM   (1UL << 18)

#define RISCV64_SIE_SSIE      (1UL << 1)
#define RISCV64_SIE_STIE      (1UL << 5)
#define RISCV64_SIE_SEIE      (1UL << 9)

#define RISCV64_SIP_SSIP      (1UL << 1)
#define RISCV64_SIP_STIP      (1UL << 5)
#define RISCV64_SIP_SEIP      (1UL << 9)

static inline uint64_t riscv64_read_sstatus(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, sstatus" : "=r"(value));
    return value;
}

static inline void riscv64_write_sstatus(uint64_t value) {
    __asm__ volatile("csrw sstatus, %0" : : "r"(value));
}

static inline uint64_t riscv64_read_stvec(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, stvec" : "=r"(value));
    return value;
}

static inline void riscv64_write_stvec(uint64_t value) {
    __asm__ volatile("csrw stvec, %0" : : "r"(value));
}

static inline uint64_t riscv64_read_sepc(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, sepc" : "=r"(value));
    return value;
}

static inline void riscv64_write_sepc(uint64_t value) {
    __asm__ volatile("csrw sepc, %0" : : "r"(value));
}

static inline uint64_t riscv64_read_scause(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, scause" : "=r"(value));
    return value;
}

static inline uint64_t riscv64_read_stval(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, stval" : "=r"(value));
    return value;
}

static inline uint64_t riscv64_read_sscratch(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, sscratch" : "=r"(value));
    return value;
}

static inline void riscv64_write_sscratch(uint64_t value) {
    __asm__ volatile("csrw sscratch, %0" : : "r"(value));
}

static inline uint64_t riscv64_read_satp(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, satp" : "=r"(value));
    return value;
}

static inline void riscv64_write_satp(uint64_t value) {
    __asm__ volatile("csrw satp, %0" : : "r"(value));
}

static inline uint64_t riscv64_read_sie(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, sie" : "=r"(value));
    return value;
}

static inline void riscv64_write_sie(uint64_t value) {
    __asm__ volatile("csrw sie, %0" : : "r"(value));
}

static inline uint64_t riscv64_read_sip(void) {
    uint64_t value;
    __asm__ volatile("csrr %0, sip" : "=r"(value));
    return value;
}

static inline uint64_t riscv64_read_time(void) {
    uint64_t value;
    __asm__ volatile("rdtime %0" : "=r"(value));
    return value;
}

static inline void riscv64_sfence_vma(void) {
    __asm__ volatile("sfence.vma zero, zero" : : : "memory");
}

#endif
