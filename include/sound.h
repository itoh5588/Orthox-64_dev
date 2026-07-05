#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

void sound_init(void);
void sound_beep_start(uint32_t freq_hz);
void sound_beep_stop(void);
int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate);

#endif
