#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

void sound_init(void);
void sound_beep_start(uint32_t freq_hz);
void sound_beep_stop(void);
/* 積んで、鳴り終わるまで待つ。起動時の自己診断が経過時間を測るのに使う */
int sound_pcm_play_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate);
/* **積むだけで戻る (D-3)。**受け取ったサンプル数を返す。0 は「いまは満杯」で
 * 失敗ではない。ゲームループから鳴らす側はこちらを使う */
int sound_pcm_submit_u8(const uint8_t* data, uint32_t len, uint32_t sample_rate);
/* 積んだぶんが鳴り終わるまで待つ (時限つき) */
void sound_pcm_drain(void);
/* **鳴り終わったブロックを無音に戻す。**タイマ割り込みから呼ぶ。
 * 呼ばないと円環が一周して同じ音がもう一度鳴る */
void sound_tick(void);

#endif
