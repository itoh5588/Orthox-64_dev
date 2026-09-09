#include <stdio.h>
#include <stdint.h>
#include "syscall.h"

int main(void) {
    uint64_t t0 = get_ticks_ms();
    printf("tick0=%lu ms\n", t0);

    sleep_ms(120);
    uint64_t t1 = get_ticks_ms();
    printf("tick1=%lu ms (delta=%lu)\n", t1, t1 - t0);

    sleep_ms(250);
    uint64_t t2 = get_ticks_ms();
    printf("tick2=%lu ms (delta=%lu)\n", t2, t2 - t1);

    return 0;
}
