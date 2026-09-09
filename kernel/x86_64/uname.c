#include "linux_syscall.h"   /* arch_uname_machine / arch_uname_version の宣言 */
#include "version.h"

/* **x86 が uname で名乗る値。**
 *
 * aarch64 は kernel/aarch64/syscall.c、riscv64 は kernel/riscv64/syscall.c に
 * 同じ 2 つを持っている。**machine は ISA そのもの**なのに、x86 のぶんだけが
 * 共通ヘッダ (include/version.h) の ORTHOX_UNAME_MACHINE = "x86_64" として
 * 置かれていた。**ISA 依存の値が共通ヘッダに残っていた**ので、他の 2 つと
 * 同じ形に揃えてここへ移した (2026-09-07)。
 *
 * 値は変えていない。x86 の uname は今までどおり machine=x86_64 /
 * version=ORTHOX_KERNEL_VERSION を返す */
const char* arch_uname_machine(void) { return "x86_64"; }
const char* arch_uname_version(void) { return ORTHOX_KERNEL_VERSION; }
