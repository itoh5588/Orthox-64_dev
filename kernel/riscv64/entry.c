#include <stdint.h>
#include "riscv64/csr.h"
#include "riscv64/entry.h"

void riscv64_prepare_initial_user_frame(riscv64_trap_frame_t* frame,
                                        uint64_t entry_pc,
                                        uint64_t user_sp,
                                        uint64_t arg0,
                                        uint64_t arg1,
                                        uint64_t arg2) {
    if (!frame) return;

    for (uint64_t i = 0; i < sizeof(*frame); i++) {
        ((uint8_t*)frame)[i] = 0;
    }

    riscv64_trap_set_user_return(frame, entry_pc, user_sp, arg0, arg1, arg2);
    frame->sstatus = riscv64_read_sstatus();
    frame->sstatus &= ~RISCV64_SSTATUS_SPP;
    frame->sstatus |= RISCV64_SSTATUS_SPIE;
    frame->sstatus |= RISCV64_SSTATUS_SUM;
}

void riscv64_prepare_fork_return_frame(riscv64_trap_frame_t* frame,
                                       uint64_t entry_pc,
                                       uint64_t user_sp) {
    riscv64_prepare_initial_user_frame(frame, entry_pc, user_sp, 0, 0, 0);
}
