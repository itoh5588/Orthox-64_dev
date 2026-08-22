#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../include/syscall.h"

#define ORTHOS_MAX_CHANNELS 32
#define ORTHOS_MIX_RATE 16000
#define ORTHOS_MIX_SAMPLES 512
#define ORTHOS_MUSIC_VOICES 16
#define ORTHOS_MUS_TICKS_PER_SEC 140
/* 音楽の持ち上げ。**1 声が最大 ±128 に正規化された後に掛ける。**
 * 大きすぎると飽和して歪み、小さすぎると聞こえない。
 * **実機の [doomsound] peak= と clip= を見て決める** */
#define ORTHOS_MUSIC_GAIN 24

// Referenced from i_sound.c when FEATURE_SOUND is enabled.
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

typedef struct {
    uint16_t sample_rate;
    uint32_t length;
    uint8_t* samples;
} orthos_cached_sound_t;

typedef struct {
    const uint8_t* data;
    uint32_t len;
} orthos_song_t;

typedef struct {
    uint8_t id[4];
    uint16_t scorelength;
    uint16_t scorestart;
    uint16_t primarychannels;
    uint16_t secondarychannels;
    uint16_t instrumentcount;
} orthos_mus_header_t;

typedef struct {
    uint8_t active;
    orthos_cached_sound_t* snd;
    uint32_t pos_q16;
    uint32_t effective_rate;
    int vol;
    int sep;
    int priority;
} orthos_channel_t;

typedef struct {
    uint8_t active;
    uint8_t released;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint8_t patch;
    uint8_t is_drum;
    uint16_t level; // 0..1024
    uint32_t phase_q32;
    uint32_t step_q32;
} orthos_music_voice_t;

typedef struct {
    const uint8_t* base;
    uint32_t len;
    uint32_t score_start;
    uint32_t cursor;
    uint8_t loop;
    uint8_t playing;
    uint32_t samples_to_next_event;
    uint8_t channel_velocity[16];
    uint8_t channel_volume[16];
    uint8_t channel_patch[16];
    uint8_t channel_pan[16];
    uint8_t channel_mod[16];
    int16_t channel_pitch[16];
    orthos_music_voice_t voices[ORTHOS_MUSIC_VOICES];
} orthos_music_state_t;

static boolean g_sound_initialized = false;
static orthos_channel_t g_channels[ORTHOS_MAX_CHANNELS];
static uint32_t g_last_update_ms = 0;
static uint32_t g_last_submit_ms = 0;
static uint8_t g_mix_buf[ORTHOS_MIX_SAMPLES];
static int32_t g_mix_acc[ORTHOS_MIX_SAMPLES];
static int g_pcm_broken = 0;
static uint32_t g_last_pcm_fail_ms = 0;
static orthos_music_state_t g_music;
/* **lump を自前で抱える。**DOOM は W_CacheLumpNum で読んだ塊をそのまま
 * 渡してくるが、寿命は zone の都合で決まる。**合成器は毎フレーム読むので、
 * 途中で解放されると踏む。**写しを持てば寿命の心配が無くなる。
 * DOOM1 shareware の音楽 lump は最大でも 30KB 程度 */
#define ORTHOS_MUSIC_MAX 65536
static uint8_t g_music_data[ORTHOS_MUSIC_MAX];
static int g_debug_logged_song = 0;
static int g_music_volume = 40;
static int32_t g_music_lp = 0;
static int g_fallback_beep_on = 0;
static uint32_t g_fallback_beep_hz = 0;
static uint32_t g_fallback_beep_deadline_ms = 0;
static uint32_t g_fallback_beep_last_trigger_ms = 0;
static int g_debug_logged_init = 0;
static int g_debug_logged_first_submit = 0;
static int g_debug_logged_first_startsound = 0;
static int g_debug_logged_init_probe = 0;

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void sfx_fallback_beep_trigger(uint32_t now) {
    int best_idx = -1;
    int best_score = -1;

    for (int i = 0; i < ORTHOS_MAX_CHANNELS; i++) {
        orthos_channel_t* c = &g_channels[i];
        if (!c->active || !c->snd || !c->snd->samples || c->snd->length == 0) {
            continue;
        }
        int score = c->vol + (c->priority / 2);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx < 0) {
        return;
    }

    orthos_channel_t* b = &g_channels[best_idx];
    // effective_rate is sample playback rate, not pitch. Downscale to
    // an audible control tone that still tracks SFX dynamics.
    uint32_t hz = clamp_u32((b->effective_rate + 8) / 16, 120, 1800);
    if (!g_fallback_beep_on || g_fallback_beep_hz != hz) {
        sound_on(hz);
        g_fallback_beep_on = 1;
        g_fallback_beep_hz = hz;
        g_fallback_beep_deadline_ms = now + 26;
    }
}

static uint32_t note_step_q32(uint8_t note, int16_t pitchbend) {
    static const uint16_t base_note_hz[12] = {
        261, 277, 293, 311, 329, 349, 369, 391, 415, 440, 466, 493
    };

    int n = (int)note;
    int oct = (n / 12) - 5;
    uint32_t freq = base_note_hz[n % 12];

    if (oct > 0) {
        while (oct-- > 0) freq <<= 1;
    } else {
        while (oct++ < 0) freq >>= 1;
    }

    // very light pitch bend support (about +/- 1 semitone span)
    freq = (uint32_t)((int32_t)freq + ((int32_t)freq * pitchbend) / 8192 / 12);
    if (freq < 20) freq = 20;
    if (freq > 4000) freq = 4000;
    return (uint32_t)(((uint64_t)freq << 32) / ORTHOS_MIX_RATE);
}

static void music_reset_state(void) {
    memset(&g_music, 0, sizeof(g_music));
    for (int i = 0; i < 16; i++) {
        g_music.channel_velocity[i] = 100;
        g_music.channel_volume[i] = 100;
        g_music.channel_patch[i] = 0;
        g_music.channel_pan[i] = 64;
        g_music.channel_mod[i] = 0;
        g_music.channel_pitch[i] = 0;
    }
    g_music_lp = 0;
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

__attribute__((unused)) static int music_parse_header(const uint8_t* data, uint32_t len, orthos_mus_header_t* h) {
    if (!data || !h || len < 16) return 0;
    h->id[0] = data[0];
    h->id[1] = data[1];
    h->id[2] = data[2];
    h->id[3] = data[3];
    h->scorelength = read_le16(data + 4);
    h->scorestart = read_le16(data + 6);
    h->primarychannels = read_le16(data + 8);
    h->secondarychannels = read_le16(data + 10);
    h->instrumentcount = read_le16(data + 12);

    if (h->id[0] != 'M' || h->id[1] != 'U' || h->id[2] != 'S' || h->id[3] != 0x1A) {
        return 0;
    }
    if ((uint32_t)h->scorestart >= len) return 0;
    return 1;
}

static int music_read_u8(uint8_t* out) {
    if (!out || !g_music.base || g_music.cursor >= g_music.len) {
        return 0;
    }
    *out = g_music.base[g_music.cursor++];
    return 1;
}

static void music_all_notes_off(void) {
    for (int i = 0; i < ORTHOS_MUSIC_VOICES; i++) {
        g_music.voices[i].released = 1;
    }
}

static void music_note_off(uint8_t channel, uint8_t note) {
    for (int i = 0; i < ORTHOS_MUSIC_VOICES; i++) {
        orthos_music_voice_t* v = &g_music.voices[i];
        if (v->active && !v->released && v->channel == channel && v->note == note) {
            v->released = 1;
        }
    }
}

static int music_pick_voice(void) {
    int free_idx = -1;
    int weakest_idx = 0;
    uint16_t weakest_level = 0xFFFF;

    for (int i = 0; i < ORTHOS_MUSIC_VOICES; i++) {
        orthos_music_voice_t* v = &g_music.voices[i];
        if (!v->active) return i;
        if (free_idx < 0 && v->released) free_idx = i;
        if (v->level < weakest_level) {
            weakest_level = v->level;
            weakest_idx = i;
        }
    }

    if (free_idx >= 0) return free_idx;
    return weakest_idx;
}

static void music_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    int idx = music_pick_voice();
    orthos_music_voice_t* v = &g_music.voices[idx];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->released = 0;
    v->channel = channel;
    v->note = note & 0x7F;
    v->velocity = velocity & 0x7F;
    v->patch = g_music.channel_patch[channel & 0x0F];
    v->is_drum = 0;
    if (v->velocity == 0) v->velocity = 1;
    v->level = 128;
    v->step_q32 = note_step_q32(v->note, g_music.channel_pitch[channel & 0x0F]);
}

static int music_decode_event_block(void) {
    while (1) {
        uint8_t event_desc = 0;
        uint8_t channel = 0;
        uint8_t event = 0;
        uint8_t b0 = 0;
        uint8_t b1 = 0;

        if (!music_read_u8(&event_desc)) {
            g_music.playing = 0;
            return 0;
        }
        channel = event_desc & 0x0F;
        event = event_desc & 0x70;

        switch (event) {
            case 0x00: // release key
                if (!music_read_u8(&b0)) {
                    g_music.playing = 0;
                    return 0;
                }
                music_note_off(channel, b0 & 0x7F);
                break;
            case 0x10: // press key
                if (!music_read_u8(&b0)) {
                    g_music.playing = 0;
                    return 0;
                }
                if (b0 & 0x80) {
                    if (!music_read_u8(&b1)) {
                        g_music.playing = 0;
                        return 0;
                    }
                    g_music.channel_velocity[channel] = b1 & 0x7F;
                }
                music_note_on(channel, b0 & 0x7F, g_music.channel_velocity[channel]);
                break;
            case 0x20: // pitch wheel
                if (!music_read_u8(&b0)) {
                    g_music.playing = 0;
                    return 0;
                }
                g_music.channel_pitch[channel] = ((int16_t)b0 * 64) - 4096;
                break;
            case 0x30: // system event
                if (!music_read_u8(&b0)) {
                    g_music.playing = 0;
                    return 0;
                }
                if (b0 >= 10 && b0 <= 14) {
                    music_all_notes_off();
                }
                break;
            case 0x40: // change controller
                if (!music_read_u8(&b0) || !music_read_u8(&b1)) {
                    g_music.playing = 0;
                    return 0;
                }
                if (b0 == 0) { // patch
                    g_music.channel_patch[channel] = b1 & 0x7F;
                } else if (b0 == 2) { // modulation
                    g_music.channel_mod[channel] = b1 & 0x7F;
                } else if (b0 == 3 || b0 == 5) { // volume/expression
                    g_music.channel_volume[channel] = b1 & 0x7F;
                } else if (b0 == 4) { // pan
                    g_music.channel_pan[channel] = b1 & 0x7F;
                }
                break;
            case 0x60: // score end
                if (g_music.loop) {
                    g_music.cursor = g_music.score_start;
                    music_all_notes_off();
                    continue;
                }
                g_music.playing = 0;
                return 0;
            default:
                g_music.playing = 0;
                return 0;
        }

        if (event_desc & 0x80) {
            uint32_t timedelay = 0;
            uint8_t working = 0;
            do {
                if (!music_read_u8(&working)) {
                    g_music.playing = 0;
                    return 0;
                }
                timedelay = timedelay * 128u + (uint32_t)(working & 0x7Fu);
            } while (working & 0x80);

            uint32_t samples = (timedelay * ORTHOS_MIX_RATE) / ORTHOS_MUS_TICKS_PER_SEC;
            g_music.samples_to_next_event = (samples > 0) ? samples : 1;
            return 1;
        }
    }
}

static int music_render_and_count_active(uint32_t count) {
    int active = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (g_music.playing) {
            while (g_music.samples_to_next_event == 0 && g_music.playing) {
                if (!music_decode_event_block()) break;
            }
        }

        int mix = 0;
        for (int vi = 0; vi < ORTHOS_MUSIC_VOICES; vi++) {
            orthos_music_voice_t* v = &g_music.voices[vi];
            if (!v->active) continue;

            uint32_t p = v->phase_q32 >> 24;
            uint32_t p2 = (v->phase_q32 + (v->phase_q32 >> 1)) >> 24;
            int mod = (int)g_music.channel_mod[v->channel] / 6;
            int vib = ((int)((p >> 5) & 0x07u) - 4) * mod;
            p = (uint32_t)((int)p + vib) & 0xFFu;

            int saw = (int)p - 128;
            int sqr = (p < 128) ? 88 : -88;
            int tri = (p < 128) ? ((int)p - 64) : (191 - (int)p);
            int pulse = ((p2 & 0xFFu) < 92u) ? 90 : -62;
            int osc = 0;

            switch (v->patch & 0x07u) {
                case 0: osc = (tri * 8 + saw * 1) / 9; break;
                case 1: osc = (tri * 6 + pulse * 2) / 8; break;
                case 2: osc = (tri * 5 + sqr * 2 + saw) / 8; break;
                case 3: osc = (saw * 5 + tri * 3) / 8; break;
                case 4: osc = (tri * 7 + pulse) / 8; break;
                case 5: osc = (saw * 4 + pulse * 2 + tri * 2) / 8; break;
                case 6: osc = (tri * 6 + saw * 2) / 8; break;
                default: osc = (tri * 7 + saw * 1) / 8; break;
            }

            v->phase_q32 += v->step_q32;

            int gain = (int)v->velocity * (int)g_music.channel_volume[v->channel] * g_music_volume;
            /* **64bit で掛ける。**
             *
             *   osc    ±128
             *   gain   velocity(<=127) x channel_volume(<=127) x 音量(<=127)
             *          = 最大 2,048,383
             *   level  <=1024
             *
             * 積は最大 2.6e11 で、**int (2.1e9) を 100 倍以上あふれる。**
             * 典型値 (velocity 100 / 音量 64) でも 8e10 で確実にあふれ、
             * **符号ごと化けてそのまま雑音になる。**実機で「音楽は鳴るが
             * 雑音が入る」の正体 (2026-08-22)。RegisterSong に
             * 「一時的に切る」と書かれていたのも、おそらくこれ。
             *
             * 分母は「全部が最大でも 1 声が ±128 に収まる」ように取り、
             * そのうえで ORTHOS_MUSIC_GAIN で持ち上げる。**定数は実測で
             * 決める** — 下の [doomsound] peak= を見ること */
            {
                int64_t num = (int64_t)osc * (int64_t)gain * (int64_t)v->level;
                int64_t den = (int64_t)127 * 127 * 127 * 1024;
                mix += (int)((num * ORTHOS_MUSIC_GAIN) / den);
            }

            if (!v->released) {
                if (v->level < 1024) {
                    v->level = (uint16_t)(v->level + 10);
                    if (v->level > 1024) v->level = 1024;
                }
            } else {
                if (v->level > 8) {
                    v->level = (uint16_t)(v->level - 8);
                } else {
                    v->active = 0;
                    v->level = 0;
                }
            }

            if (v->active) active++;
        }

        g_mix_acc[i] += mix;
        if (g_music.samples_to_next_event > 0) {
            g_music.samples_to_next_event--;
        }
    }

    return active;
}

static void get_sfx_lump_name(sfxinfo_t* sfx, char* buf, size_t len) {
    sfxinfo_t* src = (sfx->link != NULL) ? sfx->link : sfx;
    snprintf(buf, len, "ds%s", src->name);
}

static boolean cache_sound_data(sfxinfo_t* sfxinfo) {
    if (sfxinfo == NULL) {
        return false;
    }
    if (sfxinfo->driver_data != NULL) {
        return true;
    }

    int lumpnum = sfxinfo->lumpnum;
    if (lumpnum < 0) {
        return false;
    }

    uint8_t* data = (uint8_t*)W_CacheLumpNum(lumpnum, PU_STATIC);
    if (data == NULL) {
        return false;
    }
    uint32_t lumplen = (uint32_t)W_LumpLength(lumpnum);

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    uint16_t sample_rate = (uint16_t)((data[3] << 8) | data[2]);
    uint32_t length = ((uint32_t)data[7] << 24) | ((uint32_t)data[6] << 16)
                    | ((uint32_t)data[5] << 8)  | (uint32_t)data[4];

    if (length > lumplen - 8 || length <= 48) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    data += 16;
    length -= 32;

    if (length == 0 || sample_rate == 0) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    orthos_cached_sound_t* cached = (orthos_cached_sound_t*)malloc(sizeof(*cached));
    if (!cached) {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    cached->samples = (uint8_t*)malloc(length);
    if (!cached->samples) {
        free(cached);
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    memcpy(cached->samples, data + 8, length);
    cached->length = length;
    cached->sample_rate = sample_rate;

    W_ReleaseLumpNum(lumpnum);

    sfxinfo->driver_data = cached;
    return true;
}

static boolean I_OrthoS_InitSound(boolean use_sfx_prefix_param) {
    (void)use_sfx_prefix_param;
    static uint8_t init_probe[128];
    memset(g_channels, 0, sizeof(g_channels));
    g_last_update_ms = (uint32_t)get_ticks_ms();
    g_last_submit_ms = g_last_update_ms;
    g_pcm_broken = 0;
    g_last_pcm_fail_ms = 0;
    g_fallback_beep_on = 0;
    g_fallback_beep_hz = 0;
    g_fallback_beep_deadline_ms = 0;
    g_fallback_beep_last_trigger_ms = 0;
    music_reset_state();
    g_music_volume = 40;
    sound_off();
    g_sound_initialized = true;
    if (!g_debug_logged_init) {
        printf("[doomsound] init mix_rate=%d samples=%d channels=%d\n",
               ORTHOS_MIX_RATE, ORTHOS_MIX_SAMPLES, ORTHOS_MAX_CHANNELS);
        g_debug_logged_init = 1;
    }
    if (!g_debug_logged_init_probe) {
        memset(init_probe, 128, sizeof(init_probe));
        printf("[doomsound] init probe=%d\n",
               sound_pcm_u8(init_probe, (uint32_t)sizeof(init_probe), ORTHOS_MIX_RATE));
        g_debug_logged_init_probe = 1;
    }
    return true;
}

static void I_OrthoS_ShutdownSound(void) {
    sound_off();
    g_fallback_beep_on = 0;
    g_fallback_beep_hz = 0;
    g_fallback_beep_deadline_ms = 0;
    g_fallback_beep_last_trigger_ms = 0;
    g_sound_initialized = false;
}

static int I_OrthoS_GetSfxLumpNum(sfxinfo_t* sfx) {
    char namebuf[16];
    get_sfx_lump_name(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_OrthoS_UpdateSound(void) {
    if (!g_sound_initialized) return;

    uint32_t now = (uint32_t)get_ticks_ms();
    uint32_t dt_ms = now - g_last_submit_ms;
    if (dt_ms < 16) {
        if (g_fallback_beep_on && now >= g_fallback_beep_deadline_ms) {
            sound_off();
            g_fallback_beep_on = 0;
            g_fallback_beep_hz = 0;
            g_fallback_beep_deadline_ms = 0;
        }
        return;
    }
    g_last_update_ms = now;
    g_last_submit_ms = now;

    for (uint32_t i = 0; i < ORTHOS_MIX_SAMPLES; i++) g_mix_acc[i] = 0;

    int active_count = 0;

    for (int ci = 0; ci < ORTHOS_MAX_CHANNELS; ci++) {
        orthos_channel_t* c = &g_channels[ci];
        if (!c->active || !c->snd || !c->snd->samples || c->snd->length == 0) {
            continue;
        }

        uint32_t step_q16 = (uint32_t)(((uint64_t)c->effective_rate << 16) / ORTHOS_MIX_RATE);
        if (step_q16 == 0) step_q16 = 1;

        int sep_dist = c->sep > 127 ? (c->sep - 127) : (127 - c->sep);
        int pan_gain = 255 - (sep_dist * 90 / 127);  // keep center stronger
        pan_gain = clamp_int(pan_gain, 140, 255);
        int gain = clamp_int(((c->vol + 16) * pan_gain) / 255, 8, 127);

        int ch_active = 0;
        for (uint32_t i = 0; i < ORTHOS_MIX_SAMPLES; i++) {
            uint32_t idx = c->pos_q16 >> 16;
            if (idx >= c->snd->length) {
                c->active = 0;
                break;
            }

            uint32_t frac = c->pos_q16 & 0xFFFF;
            int s0 = (int)c->snd->samples[idx] - 128;
            int s1 = (idx + 1 < c->snd->length) ? ((int)c->snd->samples[idx + 1] - 128) : s0;
            int s = (int)(((int64_t)s0 * (65536 - frac) + (int64_t)s1 * frac) >> 16);

            g_mix_acc[i] += s * gain;
            c->pos_q16 += step_q16;
            ch_active = 1;
        }

        if (ch_active) active_count++;
    }

    int music_active = music_render_and_count_active(ORTHOS_MIX_SAMPLES);

    if (active_count <= 0 && music_active <= 0 && !g_music.playing) {
        sound_off();
        g_fallback_beep_on = 0;
        g_fallback_beep_hz = 0;
        g_fallback_beep_deadline_ms = 0;
        return;
    }

    int mix_sources = active_count + ((music_active > 0 || g_music.playing) ? 1 : 0);
    if (mix_sources <= 0) {
        mix_sources = 1;
    }
    int norm = 96 * mix_sources;
    int peak_lo = 255, peak_hi = 0, clipped = 0;
    for (uint32_t i = 0; i < ORTHOS_MIX_SAMPLES; i++) {
        // Tone shaping: light low-pass only (no reverb/noise).
        int x = g_mix_acc[i];
        g_music_lp += (x - g_music_lp) / 4;
        int out = 128 + (g_music_lp / norm);
        /* **飽和を数える。**clamp は歪みを隠してしまうので、
         * 「どれだけ切り落としたか」が見えないと定数を決められない */
        if (out < 0 || out > 255) clipped++;
        if (out < peak_lo) peak_lo = out;
        if (out > peak_hi) peak_hi = out;
        g_mix_buf[i] = (uint8_t)clamp_int(out, 0, 255);
    }
    /* 2 秒ごとに 1 行だけ。**振幅と飽和が分かれば ORTHOS_MUSIC_GAIN を
     * 実測で決められる。**128 が無音の中心 */
    {
        static uint32_t next_peak_ms = 0;
        static int peak_reports = 0;
        if (peak_reports < 10 && now >= next_peak_ms) {
            next_peak_ms = now + 2000;
            peak_reports++;
            printf("[doomsound] peak=%d..%d clip=%d/%d 声=%d 音源=%d 音量=%d\n",
                   peak_lo, peak_hi, clipped, (int)ORTHOS_MIX_SAMPLES,
                   music_active, mix_sources, g_music_volume);
        }
    }

    // Keep trying PCM even after failures (with cooldown), because SB16 may
    // become usable after early boot timing settles.
    if (!g_pcm_broken || (now - g_last_pcm_fail_ms) > 500) {
        int ret = sound_pcm_u8(g_mix_buf, ORTHOS_MIX_SAMPLES, ORTHOS_MIX_RATE);
        if (ret >= 0) {
            if (!g_debug_logged_first_submit) {
                printf("[doomsound] first submit=%d active=%d music=%d\n",
                       ret, active_count, (music_active > 0 || g_music.playing) ? 1 : 0);
                g_debug_logged_first_submit = 1;
            }
            g_pcm_broken = 0;
            if (g_fallback_beep_on) {
                sound_off();
                g_fallback_beep_on = 0;
                g_fallback_beep_hz = 0;
                g_fallback_beep_deadline_ms = 0;
            }
            return;
        }
        g_pcm_broken = 1;
        g_last_pcm_fail_ms = now;
    }

    // PCM unavailable: emit short SFX-priority chirps only (avoid continuous tone).
    if (g_fallback_beep_on && now >= g_fallback_beep_deadline_ms) {
        sound_off();
        g_fallback_beep_on = 0;
        g_fallback_beep_hz = 0;
        g_fallback_beep_deadline_ms = 0;
    }
    if (active_count > 0 && (now - g_fallback_beep_last_trigger_ms) >= 70) {
        sfx_fallback_beep_trigger(now);
        g_fallback_beep_last_trigger_ms = now;
    }
}

static void I_OrthoS_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= ORTHOS_MAX_CHANNELS) return;

    g_channels[channel].vol = clamp_int(vol, 0, 127);
    g_channels[channel].sep = clamp_int(sep, 0, 254);
}

static int I_OrthoS_StartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep) {
    if (!g_sound_initialized) return -1;
    if (channel < 0 || channel >= ORTHOS_MAX_CHANNELS) return -1;
    if (sfxinfo == NULL) return -1;

    if (!cache_sound_data(sfxinfo)) {
        return -1;
    }

    orthos_cached_sound_t* snd = (orthos_cached_sound_t*)sfxinfo->driver_data;
    if (!snd || !snd->samples || snd->length == 0 || snd->sample_rate == 0) {
        return -1;
    }

    int pitch = sfxinfo->pitch;
    if (pitch == 0) pitch = 128;

    uint32_t effective_rate = (uint32_t)(((uint64_t)snd->sample_rate * (uint64_t)pitch) / 128ULL);
    effective_rate = clamp_u32(effective_rate, 2000, 22050);

    g_channels[channel].active = 1;
    g_channels[channel].snd = snd;
    g_channels[channel].pos_q16 = 0;
    g_channels[channel].effective_rate = effective_rate;
    g_channels[channel].vol = clamp_int(vol, 0, 127);
    g_channels[channel].sep = clamp_int(sep, 0, 254);
    g_channels[channel].priority = sfxinfo->priority;
    if (!g_debug_logged_first_startsound) {
        printf("[doomsound] first sfx channel=%d rate=%u len=%u pitch=%d\n",
               channel, effective_rate, snd->length, pitch);
        g_debug_logged_first_startsound = 1;
    }

    return channel;
}

static void I_OrthoS_StopSound(int channel) {
    if (channel < 0 || channel >= ORTHOS_MAX_CHANNELS) return;
    g_channels[channel].active = 0;

    for (int i = 0; i < ORTHOS_MAX_CHANNELS; i++) {
        if (g_channels[i].active) return;
    }
    sound_off();
    g_fallback_beep_on = 0;
    g_fallback_beep_hz = 0;
    g_fallback_beep_deadline_ms = 0;
}

static boolean I_OrthoS_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= ORTHOS_MAX_CHANNELS) return false;
    return g_channels[channel].active ? true : false;
}

static void I_OrthoS_PrecacheSounds(sfxinfo_t* sounds, int num_sounds) {
    (void)sounds;
    (void)num_sounds;
}

static boolean I_OrthoS_InitMusic(void) { return true; }
static void I_OrthoS_ShutdownMusic(void) { g_music.playing = 0; }
static void I_OrthoS_SetMusicVolume(int volume) {
    g_music_volume = clamp_int(volume, 0, 127);
}
static void I_OrthoS_PauseSong(void) {}
static void I_OrthoS_ResumeSong(void) {}
/* MUS を受け取る。**返すのは g_music への印**で、中身は写しを持つ。
 *
 * MUS のヘッダ (16 バイト、リトルエンディアン):
 *   0  'M' 'U' 'S' 0x1A
 *   4  scoreLen    (2)
 *   6  scoreStart  (2)   ← ここから演奏データ
 *   8  channels / secondaryChannels / instrCnt / dummy
 *
 * **構造体で読まない。**詰め物と整列の前提が要るうえ、aarch64 では
 * 非整列アクセスになりうる。バイトで組み立てる */
static void* I_OrthoS_RegisterSong(void* data, int len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t score_len, score_start, total;

    g_music.playing = 0;
    g_music.base = NULL;
    g_music.len = 0;

    if (!p || len < 16) return NULL;
    if (!(p[0] == 'M' && p[1] == 'U' && p[2] == 'S' && p[3] == 0x1A)) {
        /* MID などは扱えない。**黙って落とさず 1 回だけ言う** */
        if (!g_debug_logged_song) {
            g_debug_logged_song = 1;
            printf("[doomsound] song: MUS ではない (%02x %02x %02x %02x len=%d)\n",
                   p[0], p[1], p[2], p[3], len);
        }
        return NULL;
    }

    score_len   = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
    score_start = (uint32_t)p[6] | ((uint32_t)p[7] << 8);
    if (score_start >= (uint32_t)len) return NULL;
    if (score_start + score_len > (uint32_t)len) {
        score_len = (uint32_t)len - score_start;
    }
    total = score_start + score_len;
    if (total > ORTHOS_MUSIC_MAX) {
        if (!g_debug_logged_song) {
            g_debug_logged_song = 1;
            printf("[doomsound] song: 大きすぎる %u > %u\n",
                   (unsigned)total, (unsigned)ORTHOS_MUSIC_MAX);
        }
        return NULL;
    }

    memcpy(g_music_data, p, total);
    g_music.base = g_music_data;
    g_music.len = total;
    g_music.score_start = score_start;

    if (!g_debug_logged_song) {
        g_debug_logged_song = 1;
        printf("[doomsound] song: MUS ok len=%d score=%u@%u\n",
               len, (unsigned)score_len, (unsigned)score_start);
    }
    return (void*)&g_music;
}
static void I_OrthoS_UnRegisterSong(void* handle) {
    (void)handle;
    g_music.playing = 0;
    g_music.base = NULL;
    g_music.len = 0;
    music_all_notes_off();
}
/* **ここが「一時的に切ってある」ままだった。**再生を始める関数が
 * g_music.playing = 0 を書いており、合成器 (400 行) が一度も回らなかった。
 *
 * 演奏の前に**各チャンネルの既定値を入れる。**MUS は明示するまで
 * 音量も velocity も送ってこないので、0 のままだと gain が 0 になって
 * 「鳴っているのに無音」になる */
static void I_OrthoS_PlaySong(void* handle, boolean looping) {
    int i;

    if (handle != (void*)&g_music || !g_music.base) {
        g_music.playing = 0;
        return;
    }

    for (i = 0; i < 16; i++) {
        g_music.channel_velocity[i] = 100;
        g_music.channel_volume[i]   = 127;
        g_music.channel_patch[i]    = 0;
        g_music.channel_pan[i]      = 64;
        g_music.channel_mod[i]      = 0;
        g_music.channel_pitch[i]    = 0;
    }
    for (i = 0; i < ORTHOS_MUSIC_VOICES; i++) {
        g_music.voices[i].active = 0;
        g_music.voices[i].released = 0;
        g_music.voices[i].level = 0;
        g_music.voices[i].phase_q32 = 0;
    }

    g_music.cursor = g_music.score_start;
    g_music.samples_to_next_event = 0;
    g_music.loop = looping ? 1 : 0;
    g_music.playing = 1;
}
static void I_OrthoS_StopSong(void) { g_music.playing = 0; music_all_notes_off(); }
/* **本当の状態を返す。**false 固定だと DOOM が「曲が終わった」と見て
 * 掛け直しに来る */
static boolean I_OrthoS_MusicIsPlaying(void) {
    return g_music.playing ? true : false;
}
static void I_OrthoS_PollMusic(void) {}

static snddevice_t sound_devices[] = {
    SNDDEVICE_PCSPEAKER,
    SNDDEVICE_SB,
};

static snddevice_t music_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_ADLIB,
    SNDDEVICE_GENMIDI,
};

sound_module_t DG_sound_module = {
    sound_devices,
    (int)(sizeof(sound_devices) / sizeof(sound_devices[0])),
    I_OrthoS_InitSound,
    I_OrthoS_ShutdownSound,
    I_OrthoS_GetSfxLumpNum,
    I_OrthoS_UpdateSound,
    I_OrthoS_UpdateSoundParams,
    I_OrthoS_StartSound,
    I_OrthoS_StopSound,
    I_OrthoS_SoundIsPlaying,
    I_OrthoS_PrecacheSounds,
};

music_module_t DG_music_module = {
    music_devices,
    (int)(sizeof(music_devices) / sizeof(music_devices[0])),
    I_OrthoS_InitMusic,
    I_OrthoS_ShutdownMusic,
    I_OrthoS_SetMusicVolume,
    I_OrthoS_PauseSong,
    I_OrthoS_ResumeSong,
    I_OrthoS_RegisterSong,
    I_OrthoS_UnRegisterSong,
    I_OrthoS_PlaySong,
    I_OrthoS_StopSong,
    I_OrthoS_MusicIsPlaying,
    I_OrthoS_PollMusic,
};
