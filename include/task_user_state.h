#ifndef ORTHOX_TASK_USER_STATE_H
#define ORTHOX_TASK_USER_STATE_H

#include <stdint.h>

struct arch_task_user_state {
    uint64_t entry_pc;
    uint64_t user_sp;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
};

#endif
