#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*flac_playback_allowed_cb_t)(void *context);
typedef unsigned (*flac_volume_cb_t)(void *context);
typedef void (*flac_info_cb_t)(uint32_t sample_rate, unsigned channels,
                               unsigned bits_per_sample, void *context);

esp_err_t flac_play_stream(FILE *file,
                           flac_playback_allowed_cb_t playback_allowed,
                           flac_volume_cb_t current_volume,
                           flac_info_cb_t info_ready,
                           void *context);

#ifdef __cplusplus
}
#endif
