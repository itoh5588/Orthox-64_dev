#include <stdio.h>
#include "../include/syscall.h"

int main(void) {
    printf("cpu=%d ticks=%llu\n", get_cpu_id(), (unsigned long long)get_ticks_ms());
    return 0;
}
