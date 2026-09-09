#ifndef KASSERT_H
#define KASSERT_H

#include <stdint.h>

void kernel_panic(const char *file, int line, const char *func,
                  const char *expr) __attribute__((noreturn));

#ifndef ORTHOX_ENABLE_ASSERTS
#define ORTHOX_ENABLE_ASSERTS 1
#endif

#if ORTHOX_ENABLE_ASSERTS
#define KASSERT(expr) do { \
    if (!(expr)) { \
        kernel_panic(__FILE__, __LINE__, __func__, #expr); \
    } \
} while (0)
#else
#define KASSERT(expr) do { \
    (void)sizeof(expr); \
} while (0)
#endif

#define KBUG_ON(cond) do { \
    if (cond) { \
        kernel_panic(__FILE__, __LINE__, __func__, "KBUG_ON(" #cond ")"); \
    } \
} while (0)

#endif
