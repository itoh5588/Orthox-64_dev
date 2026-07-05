#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include "syscall.h"

int main() {
    struct video_info info;
    if (get_video_info(&info) < 0) {
        printf("Failed to get video info\n");
        return 1;
    }

    printf("Video Mode: %lu x %lu, BPP: %lu, Pitch: %lu\n", info.width, info.height, info.bpp, info.pitch);

    uint32_t* vram = (uint32_t*)map_framebuffer();
    if (!vram) {
        printf("Failed to map framebuffer\n");
        return 1;
    }

    printf("VRAM mapped at %p. Filling screen with blue...\n", vram);

    // 画面を塗りつぶす (ARGB8888を想定)
    for (uint64_t y = 0; y < info.height; y++) {
        for (uint64_t x = 0; x < info.width; x++) {
            vram[y * (info.pitch / 4) + x] = 0x000000FF; // Blue
        }
    }

    printf("Done. Sleeping for 3 seconds...\n");
    for(volatile int i=0; i<100000000; i++); // 簡易待機

    return 0;
}
