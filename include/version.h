#ifndef ORTHOX_VERSION_H
#define ORTHOX_VERSION_H

#define ORTHOX_OS_NAME "Orthox-64"
#define ORTHOX_KERNEL_NAME "Orthox-64"
#define ORTHOX_KERNEL_RELEASE "0.6.0"
#define ORTHOX_KERNEL_VERSION "Orthox-64 kernel 0.6.0"

#define ORTHOX_UNAME_SYSNAME "Orthox"
#define ORTHOX_UNAME_NODENAME "orthox"
#define ORTHOX_UNAME_DOMAINNAME "localdomain"

/* **machine は ここに置かない。**ISA そのものなので arch_uname_machine() が
 * 答える (kernel/x86_64/uname.c / kernel/aarch64/syscall.c /
 * kernel/riscv64/syscall.c)。version も同じ経路。
 *
 * ---- 名乗りは 1 組だけ (2026-09-09 に決めた) ---------------------------
 *
 * **2026-09-07 まで組が 2 つ在った。**aarch64 / riscv64 の uname
 * (kernel/linux_syscall.c) だけが sysname に "Linux"、release に
 * "5.0.0-orthox" を名乗っていた。「busybox の一部が release を Linux の
 * バージョンとしてパースする」ためだったが、**上の x86 側に合わせると
 * 決めた** —— 同じ OS がアーキによって違う名前を名乗ると、その上で動く
 * プログラムがアーキごとに違う判断をする。
 *
 * **ここに 2 組目を作らない。**3 アーキとも上の ORTHOX_UNAME_* と
 * ORTHOX_KERNEL_RELEASE を使う。 */

#endif
