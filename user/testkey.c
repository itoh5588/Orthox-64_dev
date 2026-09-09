#include <stdio.h>
#include <stdint.h>
#include "syscall.h"

int main(void) {
    printf("key event test start\n");
    printf("Press some keys for ~5 seconds...\n");

    for (int i = 0; i < 500; i++) {
        struct key_event ev;
        if (get_key_event(&ev)) {
            printf("ev: pressed=%u sc=%u ascii=%u\n",
                   (unsigned)ev.pressed,
                   (unsigned)ev.scancode,
                   (unsigned)ev.ascii);
        }
        sleep_ms(10);
    }

    printf("key event test end\n");
    return 0;
}
