#ifndef ORTHOX_ARCH_BOOTMOD_H
#define ORTHOX_ARCH_BOOTMOD_H

#include <stdint.h>
#include "boot_module.h"
#include "limine.h"

/* arch_boot_module_* for x86_64: Limine の module_request を読むだけの薄い
 * inline。**Limine は x86 のブートローダ規約**なので、共有層 (fs.c) から
 * この名前が見えないようここに閉じる。定義は kernel/x86_64/init.c。 */
extern volatile struct limine_module_request module_request;

static inline uint64_t arch_boot_module_count(void) {
    return module_request.response ? module_request.response->module_count : 0;
}

/* i < arch_boot_module_count() であること (呼び手が保証する)。 */
static inline struct boot_module arch_boot_module_at(uint64_t i) {
    struct limine_file* m = module_request.response->modules[i];
    struct boot_module out;
    out.path = m->path;
    out.address = m->address;
    out.size = m->size;
    return out;
}

#endif
