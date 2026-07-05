#include <stddef.h>
#include <stdint.h>
#include "lapic.h"
#include "sys_internal.h"
#include "task.h"

static inline uint64_t rdtsc_u64(void) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid_leaf(uint32_t leaf, uint32_t subleaf,
    uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    __asm__ volatile("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static int cpu_has_rdrand(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid_leaf(1, 0, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ebx;
    (void)edx;
    return (ecx & (1U << 30)) != 0;
}

static int rdrand_u64(uint64_t* out) {
    unsigned char ok;
    uint64_t value;
    if (!out) return 0;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
    if (!ok) return 0;
    *out = value;
    return 1;
}

int64_t sys_getrandom(void* buf, size_t len, unsigned flags) {
    uint8_t* out = (uint8_t*)buf;
    static uint64_t fallback_state = 0;
    uint64_t mix = 0;
    size_t off = 0;
    (void)flags;
    if (!out) return -1;
    if (fallback_state == 0) {
        fallback_state = rdtsc_u64() ^ (lapic_get_ticks_ms() << 17) ^ 0x9E3779B97F4A7C15ULL;
    }
    while (off < len) {
        uint64_t word = 0;
        size_t take;
        if (cpu_has_rdrand() && rdrand_u64(&word)) {
            mix ^= word;
        } else {
            fallback_state ^= fallback_state >> 12;
            fallback_state ^= fallback_state << 25;
            fallback_state ^= fallback_state >> 27;
            mix ^= fallback_state * 0x2545F4914F6CDD1DULL;
        }
        mix ^= rdtsc_u64();
        mix ^= lapic_get_ticks_ms() << 9;
        mix ^= (uint64_t)(uintptr_t)get_current_task();
        word = mix;
        take = len - off;
        if (take > sizeof(word)) take = sizeof(word);
        for (size_t i = 0; i < take; i++) {
            out[off + i] = (uint8_t)(word >> (i * 8));
        }
        off += take;
    }
    return (int64_t)len;
}
