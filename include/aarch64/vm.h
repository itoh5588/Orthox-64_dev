#ifndef ORTHOX_ARCH_AARCH64_VM_H
#define ORTHOX_ARCH_AARCH64_VM_H

/* .S からも読むので、**アセンブラが解釈できない書き方をしない**。
 * C 専用の宣言は下の #ifndef __ASSEMBLER__ の中に置くこと。 */

/* カーネルの VA と物理アドレスの差 (M3b)。
 *
 * TCR_EL1.T1SZ = 25 (VA 39bit) なので、TTBR1 が受け持つのは VA[63:39] が
 * 全部 1 の範囲 = 0xffffff8000000000 以上。カーネルの VA は「物理 + この値」。
 *
 * **scripts/kernel-aarch64.ld の KERNEL_VA_OFFSET と同じ値にすること。**
 * ずれると、MMU を入れて上位 VA へ飛んだ瞬間に沈黙する。 */
#define AARCH64_KERNEL_VA_OFFSET 0xffffff8000000000

/* EL0 のコードを置く VA。**カーネルの配置とは無関係に決める。**
 * TTBR0 は M3b からユーザー専用なので、下位側のどこでもよい。
 * ユーザーのコードは PC 相対だけで書いてあるので、どの VA でも動く */
#define AARCH64_USER_VA_BASE 0x0000000000400000

#ifndef __ASSEMBLER__

#include <stdint.h>

/* 物理 <-> カーネル VA。**恒等マッピングが外れた後は必ずこれを通す。**
 *
 * MMU を入れる前は PC が物理なので、C からシンボルのアドレスを取ると
 * 物理が返る。上位 VA へ飛んだ後は同じ式が VA を返す。
 * 「いまどちらの世界に居るか」で意味が変わるので、
 * **DTB 由来のアドレス (常に物理) は必ず phys_to_virt を通すこと**。 */
static inline uint64_t aarch64_phys_to_virt(uint64_t pa) {
    return pa + AARCH64_KERNEL_VA_OFFSET;
}

static inline uint64_t aarch64_virt_to_phys(uint64_t va) {
    return va - AARCH64_KERNEL_VA_OFFSET;
}

uint64_t aarch64_vm_user_root_pa_current(void);

/* いま上位 VA で走っているか。恒等マッピングを外してよいかの判断に使う */
static inline int aarch64_vm_running_high(void) {
    uint64_t pc;
    __asm__ volatile("adr %0, ." : "=r"(pc));
    return (pc >> 39) == 0x1ffffffULL;
}

#endif /* __ASSEMBLER__ */

#endif
