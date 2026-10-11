#include <stddef.h>
#include <stdint.h>

/* ---- memset / memcpy / memmove (2026-10-11) ------------------------------
 *
 * **8 バイトずつ扱う。** 以前は 1 バイト 1 周 (lbu/sb と 4 命令) で、
 * Arty S7 (MicroBlaze V) ではページ 1 枚のコピーかゼロ埋めに約 2 万 4 千命令
 * かかり、fork と exec の時間の大半がここだった。
 *
 * 送り元と送り先の 8 バイト境界のずれが同じときだけ語で扱い、端はバイトで
 * 埋める。ずれが違うときはバイトのまま (aarch64 は -mstrict-align で、
 * 揃っていない語アクセスを出せない)。語は may_alias の型で読み書きする。
 *
 * **ループを memcpy の呼び出しに置き換えさせない。** -ffreestanding
 * (= -fno-builtin) なので、コンパイラはこのループを memcpy/memset と
 * 見なさない (置き換えられると自分自身を呼んで止まらない)。
 * ビルドしたら逆アセンブルで自己呼び出しが無いことを見ること */
typedef uint64_t __attribute__((may_alias)) cstring_word_t;

void* memset(void* dst, int c, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    uint64_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;
    v |= v << 32;
    while (n && ((uintptr_t)p & 7)) {
        *p++ = (uint8_t)c;
        n--;
    }
    {
        cstring_word_t* w = (cstring_word_t*)p;
        for (; n >= 32; n -= 32, w += 4) {
            w[0] = v;
            w[1] = v;
            w[2] = v;
            w[3] = v;
        }
        for (; n >= 8; n -= 8) *w++ = v;
        p = (uint8_t*)w;
    }
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        {
            cstring_word_t* wd = (cstring_word_t*)d;
            const cstring_word_t* ws = (const cstring_word_t*)s;
            for (; n >= 32; n -= 32, wd += 4, ws += 4) {
                uint64_t a = ws[0], b = ws[1], c = ws[2], e = ws[3];
                wd[0] = a;
                wd[1] = b;
                wd[2] = c;
                wd[3] = e;
            }
            for (; n >= 8; n -= 8) *wd++ = *ws++;
            d = (uint8_t*)wd;
            s = (const uint8_t*)ws;
        }
    }
    while (n--) *d++ = *s++;
    return dst;
}

/* 前から写して良い (d < s か重ならない) なら memcpy と同じ。後ろからは
 * 語で読んでから語で書くので、重なっていても読む前に潰さない */
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    d += n;
    s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        {
            cstring_word_t* wd = (cstring_word_t*)d;
            const cstring_word_t* ws = (const cstring_word_t*)s;
            for (; n >= 8; n -= 8) *--wd = *--ws;
            d = (uint8_t*)wd;
            s = (const uint8_t*)ws;
        }
    }
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

char* strcpy(char* dst, const char* src) {
    size_t i = 0;
    while (src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
