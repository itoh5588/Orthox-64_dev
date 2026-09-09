#include <stdint.h>
#include "syscall.h"

/* **x86 の syscall 命令の入口設定。**2026-09-07 に kernel/syscall.c から移した。
 * MSR は x86 の機構なので、**syscall のディスパッチ (どの番号をどの実装へ
 * 送るか) と同じファイルに置いてはいけない。**中身は 1 行も変えていない。
 *
 *   EFER.SCE   syscall/sysret を有効にする
 *   STAR       syscall で載る CS/SS の選択子 (カーネル 0x08 / ユーザ 0x10)
 *   LSTAR      syscall が飛ぶ先 = syscall_entry (kernel/x86_64/syscall_entry.S)
 *   SFMASK     syscall 時に落とす RFLAGS のビット (0x200 = IF)
 *
 * aarch64 / riscv64 では対応する設定は例外ベクタ側 (VBAR_EL1 / stvec) に
 * あり、kernel/aarch64/boot.c と kernel/riscv64/trap.c が持っている */

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

extern void syscall_entry(void);

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t low = val & 0xFFFFFFFF;
    uint32_t high = val >> 32;
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_init_cpu(void) {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);
    uint64_t star = (0x10ULL << 48) | (0x08ULL << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);
}

void syscall_init(void) {
    syscall_init_cpu();
}

