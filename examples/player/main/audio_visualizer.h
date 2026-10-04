#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_VISUALIZER_POINTS 32U

void audio_visualizer_submit_q31(const int32_t *stereo_pcm,
                                 size_t frame_count);
void audio_visualizer_get_waveform(int16_t *points, size_t point_count);

#ifdef __cplusplus
}
#endif
