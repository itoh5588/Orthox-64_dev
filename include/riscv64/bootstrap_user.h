#ifndef ORTHOX_RISCV64_BOOTSTRAP_USER_H
#define ORTHOX_RISCV64_BOOTSTRAP_USER_H

#include <stddef.h>

int riscv64_bootstrap_user_file_data(const char* path, void** data, size_t* size);

#endif
