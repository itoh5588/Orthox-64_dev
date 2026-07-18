#ifndef ORTHOX_ARCH_RISCV64_SBI_H
#define ORTHOX_ARCH_RISCV64_SBI_H

#include <stdint.h>

#define RISCV64_SBI_EXT_TIME 0x54494D45UL
#define RISCV64_SBI_EXT_BASE 0x10UL

#define RISCV64_SBI_FID_SET_TIMER 0UL

typedef struct riscv64_sbi_ret {
    long error;
    long value;
} riscv64_sbi_ret_t;

static inline riscv64_sbi_ret_t riscv64_sbi_ecall(uint64_t ext,
                                                  uint64_t fid,
                                                  uint64_t arg0,
                                                  uint64_t arg1,
                                                  uint64_t arg2,
                                                  uint64_t arg3,
                                                  uint64_t arg4,
                                                  uint64_t arg5) {
    register uint64_t a0 __asm__("a0") = arg0;
    register uint64_t a1 __asm__("a1") = arg1;
    register uint64_t a2 __asm__("a2") = arg2;
    register uint64_t a3 __asm__("a3") = arg3;
    register uint64_t a4 __asm__("a4") = arg4;
    register uint64_t a5 __asm__("a5") = arg5;
    register uint64_t a6 __asm__("a6") = fid;
    register uint64_t a7 __asm__("a7") = ext;
    __asm__ volatile("ecall"
                     : "+r"(a0), "+r"(a1)
                     : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                     : "memory");
    riscv64_sbi_ret_t ret = { (long)a0, (long)a1 };
    return ret;
}

static inline riscv64_sbi_ret_t riscv64_sbi_set_timer(uint64_t stime_value) {
    return riscv64_sbi_ecall(RISCV64_SBI_EXT_TIME, RISCV64_SBI_FID_SET_TIMER,
                             stime_value, 0, 0, 0, 0, 0);
}

#endif
