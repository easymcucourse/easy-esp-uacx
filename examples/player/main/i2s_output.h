#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t i2s_output_init(void);
esp_err_t i2s_output_set_sample_rate(uint32_t sample_rate);
esp_err_t i2s_output_write(const int32_t *stereo_pcm, size_t frame_count);
