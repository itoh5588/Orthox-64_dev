/*
 * Orthox の私物 syscall (ORTH_SYS_*) を aarch64 から呼ぶ薄い層。
 *
 * **user/syscalls.c は x86 専用。**あちらは `syscall` 命令と rdi/rsi/rdx を
 * 直書きしていて aarch64 では通らない。DOOM が使う 5 本だけをここに置く。
 *
 * aarch64 の呼び出し規約は asm-generic:
 *   x8 = 番号、x0-x5 = 引数、svc #0、戻りは x0
 *
 * **番号と struct の形は x86 と揃えてある** (include/syscall.h) ので、
 * doomgeneric 側のコードは書き換えずに済む。
 */
#include <stdint.h>
#include "../include/syscall.h"

static int64_t orth_svc1(uint64_t num, uint64_t a0) {
    register uint64_t x8 __asm__("x8") = num;
    register uint64_t x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

static int64_t orth_svc0(uint64_t num) {
    register uint64_t x8 __asm__("x8") = num;
    register uint64_t x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

int get_video_info(struct video_info* info) {
    return (int)orth_svc1(ORTH_SYS_GET_VIDEO_INFO, (uint64_t)(uintptr_t)info);
}

uint64_t map_framebuffer(void) {
    return (uint64_t)orth_svc0(ORTH_SYS_MAP_FRAMEBUFFER);
}

uint64_t get_ticks_ms(void) {
    return (uint64_t)orth_svc0(ORTH_SYS_GET_TICKS_MS);
}

int sleep_ms(uint64_t ms) {
    return (int)orth_svc1(ORTH_SYS_SLEEP_MS, ms);
}

int get_key_event(struct key_event* ev) {
    return (int)orth_svc1(ORTH_SYS_GET_KEY_EVENT, (uint64_t)(uintptr_t)ev);
}

/* ---- 音 ------------------------------------------------------------------
 *
 * **Pi 4 に音のドライバはまだ無い。**それでも syscall として出す —
 * カーネル側が ENOSYS を返し、「実装していない番号」として起動ログに
 * 1 度だけ出る。**ここで黙って 0 を返すと、音が出ない理由が分からなくなる。**
 */
int sound_on(uint32_t freq_hz) {
    return (int)orth_svc1(ORTH_SYS_SOUND_ON, freq_hz);
}

int sound_off(void) {
    return (int)orth_svc0(ORTH_SYS_SOUND_OFF);
}

int sound_pcm_u8(const uint8_t* samples, uint32_t count, uint32_t sample_rate) {
    register uint64_t x8 __asm__("x8") = ORTH_SYS_SOUND_PCM_U8;
    register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)samples;
    register uint64_t x1 __asm__("x1") = count;
    register uint64_t x2 __asm__("x2") = sample_rate;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return (int)(int64_t)x0;
}
