#ifndef ORTHOX_ARCH_AARCH64_TRAP_H
#define ORTHOX_ARCH_AARCH64_TRAP_H

/* 例外フレーム (M3c-2a)。
 *
 * **kernel/aarch64/vectors.S の SAVE_ALL と対で決まっている。**
 * どちらかを変えたらもう一方も変えること。下の _Static_assert が
 * 大きさのずれだけは捕まえる。
 *
 *   offset  中身
 *   0..240  x0..x30          (31 本)
 *   248     ELR_EL1          例外からの戻り先
 *   256     SPSR_EL1         戻り先の状態
 *   264     SP_EL0           **ユーザーのスタックポインタ**
 *   計 272 バイト (16 バイト境界)
 *
 * **SP_EL0 がフレームに要る理由。** AArch64 は EL0 と EL1 で sp が
 * 別レジスタなので、riscv64 のように「フレームの sp」が自動では入らない。
 * 入れておかないと、execve / fork がユーザーのスタックを組み替えられない
 * (arch_syscall_set_stack_pointer が書き込む先が無い)。
 * SAVE_ALL は元から 272 バイト取っていて 264 が余っていたので、
 * 枠を増やさずに入った。
 */

#define AARCH64_FRAME_X30    (30 * 8)
#define AARCH64_FRAME_ELR    (31 * 8)
#define AARCH64_FRAME_SPSR   (32 * 8)
#define AARCH64_FRAME_SP_EL0 (33 * 8)
#define AARCH64_FRAME_SIZE   272

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct aarch64_trap_frame {
    uint64_t x[31];     /* x0..x30 */
    uint64_t elr;       /* ELR_EL1 */
    uint64_t spsr;      /* SPSR_EL1 */
    uint64_t sp_el0;    /* ユーザーの sp */
} aarch64_trap_frame_t;

_Static_assert(sizeof(aarch64_trap_frame_t) == AARCH64_FRAME_SIZE,
               "aarch64_trap_frame_t と vectors.S の SAVE_ALL がずれている");

/* svc の番号とシステムコール引数の置き場。**riscv64 は a7 / a0-a5、
 * AArch64 は x8 / x0-x5。** 番号のレジスタだけ離れている */
#define AARCH64_FRAME_NR_REG   8
#define AARCH64_FRAME_ARG_REG  0

/* EL1h (SP_EL1 を使う EL1) / EL0t (SP_EL0 を使う EL0) の SPSR */
#define AARCH64_SPSR_EL1H   0x00000005ULL
#define AARCH64_SPSR_EL0T   0x00000000ULL

/* カーネルスタックの入れ替え。riscv64 の riscv64_trap_set_kernel_stack 相当。
 * **AArch64 では例外で自動的に SP_EL1 に切り替わる**ので、riscv64 のように
 * sscratch を手で入れ替える必要が無い。切り替え先を覚えておくだけ */
void aarch64_trap_set_kernel_stack(uint64_t kernel_sp);
uint64_t aarch64_trap_kernel_stack(void);

#endif /* __ASSEMBLER__ */

#endif
