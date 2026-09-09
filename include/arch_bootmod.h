#ifndef ORTHOX_ARCH_BOOTMOD_SELECT_H
#define ORTHOX_ARCH_BOOTMOD_SELECT_H

#if defined(__aarch64__)
#include "aarch64/bootmod.h"
#elif defined(__x86_64__)
#include "x86_64/bootmod.h"
#else
#error "Unsupported architecture for arch_bootmod.h"
#endif

#endif
