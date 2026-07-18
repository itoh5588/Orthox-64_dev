#ifndef ORTHOX_ARCH_TASK_SELECT_H
#define ORTHOX_ARCH_TASK_SELECT_H

#if defined(__riscv)
#include "riscv64/task.h"
#elif defined(__x86_64__)
#include "x86_64/task.h"
#else
#error "Unsupported architecture for arch_task.h"
#endif

#endif
