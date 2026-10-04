#include "audio_visualizer.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define SPECTRUM_SAMPLE_COUNT    64U
#define SPECTRUM_TABLE_SCALE     16384
#define SPECTRUM_MAX_AMPLITUDE   1000
#define SPECTRUM_TARGET_PEAK     920
#define SPECTRUM_NOISE_FLOOR     6
#define SPECTRUM_ATTACK_NUM      3
#define SPECTRUM_ATTACK_DEN      4
#define SPECTRUM_RELEASE_NUM     5
#define SPECTRUM_RELEASE_DEN     8
#define TWO_PI                   6.28318530717958647692f

static portMUX_TYPE s_visualizer_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_spectrum[AUDIO_VISUALIZER_POINTS];
static int16_t s_cos_table[AUDIO_VISUALIZER_POINTS][SPECTRUM_SAMPLE_COUNT];
static int16_t s_sin_table[AUDIO_VISUALIZER_POINTS][SPECTRUM_SAMPLE_COUNT];
static bool s_tables_ready;

static int16_t clamp_spectrum_sample(int32_t sample)
{
    if (sample > SPECTRUM_MAX_AMPLITUDE) {
        return SPECTRUM_MAX_AMPLITUDE;
    }
    if (sample < 0) {
        return 0;
    }
    return (int16_t)sample;
}

static void init_tables_once(void)
{
    if (s_tables_ready) {
        return;
    }

    for (size_t band = 0; band < AUDIO_VISUALIZER_POINTS; ++band) {
        const float bin = (float)(band + 1U);
        for (size_t sample = 0; sample < SPECTRUM_SAMPLE_COUNT; ++sample) {
            const float angle =
                TWO_PI * bin * (float)sample / (float)SPECTRUM_SAMPLE_COUNT;
            s_cos_table[band][sample] =
                (int16_t)lrintf(cosf(angle) * (float)SPECTRUM_TABLE_SCALE);
            s_sin_table[band][sample] =
                (int16_t)lrintf(sinf(angle) * (float)SPECTRUM_TABLE_SCALE);
        }
    }
    s_tables_ready = true;
}

static int32_t abs_i64_to_i32(int64_t value)
{
    if (value < 0) {
        value = -value;
    }
    return value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

void audio_visualizer_submit_q31(const int32_t *stereo_pcm,
                                 size_t frame_count)
{
    if (stereo_pcm == NULL || frame_count == 0) {
        return;
    }
    init_tables_once();

    int16_t samples[SPECTRUM_SAMPLE_COUNT];
    int32_t dc = 0;
    for (size_t sample = 0; sample < SPECTRUM_SAMPLE_COUNT; ++sample) {
        const size_t frame = sample * frame_count / SPECTRUM_SAMPLE_COUNT;
        const int32_t left = stereo_pcm[frame * 2];
        const int32_t right = stereo_pcm[frame * 2 + 1];
        const int32_t mono = (left / 2) + (right / 2);
        const int16_t sample_16 = (int16_t)(mono / 65536);
        samples[sample] = sample_16;
        dc += sample_16;
    }
    dc /= (int32_t)SPECTRUM_SAMPLE_COUNT;

    int32_t magnitudes[AUDIO_VISUALIZER_POINTS];
    int32_t peak = 0;
    for (size_t band = 0; band < AUDIO_VISUALIZER_POINTS; ++band) {
        int64_t real = 0;
        int64_t imag = 0;
        for (size_t sample = 0; sample < SPECTRUM_SAMPLE_COUNT; ++sample) {
            const int32_t centered = (int32_t)samples[sample] - dc;
            real += (int64_t)centered * s_cos_table[band][sample];
            imag -= (int64_t)centered * s_sin_table[band][sample];
        }

        int32_t magnitude =
            (abs_i64_to_i32(real) + abs_i64_to_i32(imag)) >>
            (14 + 3);
        if (magnitude < SPECTRUM_NOISE_FLOOR) {
            magnitude = 0;
        }
        magnitudes[band] = magnitude;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    int16_t next_spectrum[AUDIO_VISUALIZER_POINTS];
    for (size_t band = 0; band < AUDIO_VISUALIZER_POINTS; ++band) {
        const int32_t scaled =
            peak > 0 ? magnitudes[band] * SPECTRUM_TARGET_PEAK / peak : 0;
        next_spectrum[band] = clamp_spectrum_sample(scaled);
    }

    portENTER_CRITICAL(&s_visualizer_lock);
    for (size_t band = 0; band < AUDIO_VISUALIZER_POINTS; ++band) {
        const int32_t previous = s_spectrum[band];
        const int32_t target = next_spectrum[band];
        if (target > previous) {
            s_spectrum[band] =
                (int16_t)((previous +
                           target * SPECTRUM_ATTACK_NUM) /
                          SPECTRUM_ATTACK_DEN);
        } else {
            s_spectrum[band] =
                (int16_t)(previous * SPECTRUM_RELEASE_NUM /
                          SPECTRUM_RELEASE_DEN);
        }
    }
    portEXIT_CRITICAL(&s_visualizer_lock);
}

void audio_visualizer_get_waveform(int16_t *points, size_t point_count)
{
    if (points == NULL || point_count == 0) {
        return;
    }

    portENTER_CRITICAL(&s_visualizer_lock);
    const size_t copy_count = point_count < AUDIO_VISUALIZER_POINTS
                                  ? point_count
                                  : AUDIO_VISUALIZER_POINTS;
    memcpy(points, s_spectrum, copy_count * sizeof(points[0]));
    portEXIT_CRITICAL(&s_visualizer_lock);

    for (size_t index = AUDIO_VISUALIZER_POINTS; index < point_count;
         ++index) {
        points[index] = 0;
    }
}
