#include <stdio.h>
#include <stdint.h>
#include "syscall.h"

int main(void) {
    printf("sound test start\n");

    uint32_t notes[] = {440, 660, 880, 660, 440};
    for (unsigned i = 0; i < sizeof(notes) / sizeof(notes[0]); i++) {
        sound_on(notes[i]);
        sleep_ms(120);
        sound_off();
        sleep_ms(60);
    }

    printf("pcm test start\n");
    uint8_t pcm[16000 / 4];
    for (unsigned i = 0; i < sizeof(pcm); i++) {
        pcm[i] = ((i / 16) & 1U) ? 224U : 32U;
    }
    int played = sound_pcm_u8(pcm, (uint32_t)sizeof(pcm), 16000);
    printf("pcm submitted=%d\n", played);
    sleep_ms(300);
    sound_off();

    printf("sound test end\n");
    return 0;
}
