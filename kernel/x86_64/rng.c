#include <stddef.h>
#include <stdint.h>
#include "lapic.h"
#include "linux_syscall.h"   /* arch_random_fill の宣言 */
#include "stdio.h"           /* 乱数源が無いときの警告 */
#include "task.h"

/* **x86 の乱数材料。**2026-09-07 に kernel/sys_random.c から移した。
 * rdtsc / cpuid / rdrand は x86 の命令なので、**syscall 層に置いてはいけない。**
 * アルゴリズムは 1 行も変えていない (移動だけ)。
 *
 * **2026-09-08 に arch_random_bytes を足した。**getrandom(2) はそちらを使う
 * ようにした —— 「乱数源が無ければ無いと答える」ほうへ 3 アーキとも揃えた。
 * arch_random_fill は残してあるが、いま呼び手は居ない (下のコメント参照)。 */

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

void arch_random_fill(void* buf, size_t len) {
    uint8_t* out = (uint8_t*)buf;
    static uint64_t fallback_state = 0;
    static int have_rdrand = -1;   /* cpuid は 1 度だけ (下のループは 8 バイト刻み) */
    static int warned = 0;
    uint64_t mix = 0;
    size_t off = 0;
    if (!out) return;
    if (have_rdrand < 0) have_rdrand = cpu_has_rdrand();
    if (!have_rdrand && !warned) {
        warned = 1;
        puts("[rng] no RDRAND; /dev/urandom is mixed, not random\r\n");
    }
    if (fallback_state == 0) {
        fallback_state = rdtsc_u64() ^ (lapic_get_ticks_ms() << 17) ^ 0x9E3779B97F4A7C15ULL;
    }
    while (off < len) {
        uint64_t word = 0;
        size_t take;
        if (have_rdrand && rdrand_u64(&word)) {
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
}

/* **機械の乱数源だけを使う (2026-09-08)。**RDRAND が無い / 失敗する CPU では
 * -1 を返し、**足りない分を作らない。**呼び手 (getrandom) が ENOSYS を返す。
 *
 * aarch64 (kernel/aarch64/rng.c) と同じ契約。**混ぜ物で長さだけ揃えるのは
 * arch_random_fill の側の仕事**で、そちらは乱数源が無い機械でも必ず埋める。 */
int64_t arch_random_bytes(void* buf, size_t len) {
    uint8_t* out = (uint8_t*)buf;
    size_t off = 0;
    if (!out) return -1;
    if (len == 0) return 0;
    if (!cpu_has_rdrand()) return -1;
    while (off < len) {
        uint64_t word;
        size_t take;
        int tries = 10;   /* RDRAND は稀に失敗する。Intel の推奨は 10 回 */
        while (tries-- > 0 && !rdrand_u64(&word)) { }
        if (tries < 0) return (off > 0) ? (int64_t)off : -1;
        take = len - off;
        if (take > sizeof(word)) take = sizeof(word);
        for (size_t i = 0; i < take; i++) out[off + i] = (uint8_t)(word >> (i * 8));
        off += take;
    }
    return (int64_t)len;
}
