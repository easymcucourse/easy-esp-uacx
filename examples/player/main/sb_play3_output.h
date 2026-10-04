#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t sb_play3_output_init(void);
esp_err_t sb_play3_output_prepare(uint32_t sample_rate);
bool sb_play3_output_is_active(void);
esp_err_t sb_play3_output_write_q31(const int32_t *stereo_pcm,
                                    size_t frame_count);
