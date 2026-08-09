#ifndef ORTHOX_ARCH_AARCH64_SYSCALL_H
#define ORTHOX_ARCH_AARCH64_SYSCALL_H

/* 共有システムコール層から見たフレームの読み書き (M3c-2a)。
 *
 * riscv64 との違いは 2 つだけ:
 *
 *   |            | riscv64 | AArch64            |
 *   |------------|---------|--------------------|
 *   | 番号       | a7      | **x8**             |
 *   | 引数       | a0-a5   | x0-x5              |
 *   | 戻り値     | a0      | x0                 |
 *   | 戻り先     | sepc    | ELR_EL1            |
 *   | ユーザー sp | sp     | **SP_EL0 (別枠)**  |
 *
 * 番号のレジスタが引数の並びから離れているのが AArch64。riscv64 は
 * a0-a7 が連続しているので a7 も同じ配列で取れる。
 */

#include <stdint.h>
#include "aarch64/trap.h"

struct arch_task_context;

typedef aarch64_trap_frame_t arch_syscall_frame_t;

void aarch64_syscall_dispatch(aarch64_trap_frame_t* frame);
void aarch64_syscall_sync_current_user_frame(const aarch64_trap_frame_t* frame);
void aarch64_syscall_set_current_context(struct arch_task_context* ctx);
int aarch64_console_echo_enabled(void);

static inline uint64_t arch_syscall_number(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[AARCH64_FRAME_NR_REG] : 0;
}

static inline uint64_t arch_syscall_arg0(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[0] : 0;
}

static inline uint64_t arch_syscall_arg1(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[1] : 0;
}

static inline uint64_t arch_syscall_arg2(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[2] : 0;
}

static inline uint64_t arch_syscall_arg3(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[3] : 0;
}

static inline uint64_t arch_syscall_arg4(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[4] : 0;
}

static inline uint64_t arch_syscall_arg5(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[5] : 0;
}

static inline void arch_syscall_set_number(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[AARCH64_FRAME_NR_REG] = value;
}

static inline void arch_syscall_set_arg0(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[0] = value;
}

static inline void arch_syscall_set_arg1(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[1] = value;
}

static inline void arch_syscall_set_arg2(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[2] = value;
}

static inline void arch_syscall_set_arg3(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[3] = value;
}

static inline void arch_syscall_set_arg4(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[4] = value;
}

static inline void arch_syscall_set_arg5(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[5] = value;
}

static inline void arch_syscall_set_return(arch_syscall_frame_t* frame, uint64_t value) {
    if (!frame) return;
    frame->x[0] = value;
}

static inline uint64_t arch_syscall_return(const arch_syscall_frame_t* frame) {
    return frame ? frame->x[0] : 0;
}

/* **svc の ELR_EL1 は svc の次の命令を指している。** riscv64 の sepc が
 * ecall 自身を指していて +4 が要るのと逆 (日報2026-08-09 追2-2 で実証済み) */
static inline uint64_t arch_syscall_program_counter(const arch_syscall_frame_t* frame) {
    return frame ? frame->elr : 0;
}

static inline void arch_syscall_set_program_counter(arch_syscall_frame_t* frame, uint64_t pc) {
    if (!frame) return;
    frame->elr = pc;
}

/* **SP_EL0。** フレームの x[] には入っていない (EL1 実行中は SP_EL1 を
 * 使うため)。SAVE_ALL が別枠で積んでいる */
static inline uint64_t arch_syscall_stack_pointer(const arch_syscall_frame_t* frame) {
    return frame ? frame->sp_el0 : 0;
}

static inline void arch_syscall_set_stack_pointer(arch_syscall_frame_t* frame, uint64_t sp) {
    if (!frame) return;
    frame->sp_el0 = sp;
}

#endif
