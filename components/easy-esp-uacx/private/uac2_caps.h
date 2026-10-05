/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "uac2_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UAC2_BIT_DEPTH_16  (1u << 0)
#define UAC2_BIT_DEPTH_24  (1u << 1)
#define UAC2_BIT_DEPTH_32  (1u << 2)

/** Capabilities of a DAC at ONE USB speed (bandwidth-limited). */
typedef struct {
    /* PCM */
    bool     supports_pcm;
    uint32_t min_pcm_rate;
    uint32_t max_pcm_rate;
    uint8_t  bit_depth_mask;     /* UAC2_BIT_DEPTH_* */
    uint8_t  max_channels;

    /* DSD */
    bool     supports_dsd;        /* native or DoP */
    bool     supports_dsd_dop;
    bool     supports_dsd_native;
    uac2_dsd_rate_t max_dsd_rate; /* 0 if none */
} uac2_speed_caps_t;

/** DAC capabilities, split by Full-Speed and High-Speed host links. */
typedef struct {
    uac2_speed_caps_t fs;
    uac2_speed_caps_t hs;
} uac2_dac_caps_t;

/**
 * @brief Evaluate DAC capabilities for both FS (1023 B/frame) and HS (3072 B/microframe).
 * @return ESP_OK, or ESP_ERR_INVALID_ARG on NULL.
 */
esp_err_t uac2_get_dac_caps(const uac2_device_caps_t *dac, uac2_dac_caps_t *out_caps);

/** Single-speed evaluation against an arbitrary host. */
esp_err_t uac2_get_speed_caps(const uac2_device_caps_t *dac, const uac2_host_hw_caps_t *host,
                              uac2_speed_caps_t *out);
#ifdef __cplusplus
}
#endif
