#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_output_init(void);
esp_err_t audio_output_set_sample_rate(uint32_t sample_rate);
/* volume_permille is an amplitude gain from 0 (mute) to 1000 (100.0%). */
esp_err_t audio_output_write_s16(const int16_t *pcm, size_t frame_count,
                                 unsigned channels,
                                 unsigned volume_permille);
esp_err_t audio_output_write_q31(const int32_t *pcm, size_t frame_count,
                                 unsigned channels,
                                 unsigned volume_permille);
void audio_output_get_status_text(char *buffer, size_t buffer_size);
esp_err_t audio_output_silence(size_t frame_count);
esp_err_t audio_output_test_tone(void);

#ifdef __cplusplus
}
#endif
