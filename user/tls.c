#include <stddef.h>
#include <stdint.h>

extern void __init_tls(size_t *auxv);

void _tls_init(uint64_t *auxv) {
    if (!auxv) return;
    __init_tls((size_t *)auxv);
}
