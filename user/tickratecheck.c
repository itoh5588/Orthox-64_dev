#include <stdio.h>
#include <stdint.h>
#include "syscall.h"

int main(void) {
    uint64_t start = get_ticks_ms();
    printf("tickratecheck start=%lu\n", start);

    for (int i = 0; i < 5; ++i) {
        sleep_ms(1000);
        uint64_t now = get_ticks_ms();
        printf("tickratecheck step=%d now=%lu delta=%lu\n", i + 1, now, now - start);
    }

    return 0;
}
