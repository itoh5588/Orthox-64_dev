#ifndef ORTHOX_ARCH_BOOTMOD_H
#define ORTHOX_ARCH_BOOTMOD_H

#include <stdint.h>
#include "boot_module.h"

/* arch_boot_module_* for aarch64: **ブートローダが載せる初期ファイルは無い。**
 *
 * Pi 4 も QEMU virt も Limine を使わず、root は xv6fs のイメージ
 * (virtio-blk / EMMC2) から来る。以前は kernel/aarch64/stubs.c に空の
 * limine_module_request を置いて共有層と名前を合わせていたが、
 * **x86 のブートローダ規約の名前を ARM 側が名乗る形**だったのでやめた。 */
static inline uint64_t arch_boot_module_count(void) {
    return 0;
}

static inline struct boot_module arch_boot_module_at(uint64_t i) {
    struct boot_module out;
    (void)i;
    out.path = 0;
    out.address = 0;
    out.size = 0;
    return out;
}

#endif
