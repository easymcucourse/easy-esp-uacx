/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uac2_device *uac2_device_handle_t;

typedef enum {
    UAC2_STATE_DETACHED = 0,
    UAC2_STATE_ATTACHED,
    UAC2_STATE_ENUMERATING,
    UAC2_STATE_READY,
    UAC2_STATE_CONFIGURING,
    UAC2_STATE_STREAMING,
    UAC2_STATE_STOPPING,
    UAC2_STATE_CLEANUP,
} uac2_state_t;

typedef enum {
    UAC2_FORMAT_PCM = 0,
    UAC2_FORMAT_DSD_DOP,
    UAC2_FORMAT_DSD_NATIVE,
} uac2_format_t;

/** Compact playback stream capability (one per usable alt setting). */
typedef struct {
    uint8_t  interface_num;
    uint8_t  alt_setting;
    uint8_t  ep_addr;
    uint16_t max_packet_size;
    uint8_t  channels;
    uint8_t  subslot_size;
    uint8_t  bit_resolution;
    uint8_t  clock_id;
    uint32_t rate_min;
    uint32_t rate_max;
    uint8_t  flags;          /* UAC2_CAP_FLAG_* */
} uac2_stream_cap_t;

#define UAC2_CAP_FLAG_PCM         (1u << 0)
#define UAC2_CAP_FLAG_RAW_DATA    (1u << 1)  /* Type I RAW_DATA, candidate for native DSD */
#define UAC2_CAP_FLAG_ASYNC       (1u << 2)
#define UAC2_CAP_FLAG_FEEDBACK    (1u << 3)

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t  num_playback_caps;
    uac2_stream_cap_t *playback_caps;
    uint32_t feature_flags;  /* UAC2_QUIRK_* resolved flags */
} uac2_device_caps_t;

typedef struct {
    uint8_t  max_speed;          /* 0 = FS, 1 = HS */
    uint32_t max_iso_payload;    /* bytes per (micro)frame */
    bool     high_bandwidth_iso;
} uac2_host_hw_caps_t;

typedef enum {
    UAC2_DSD64 = 64, UAC2_DSD128 = 128, UAC2_DSD256 = 256,
} uac2_dsd_rate_t;

typedef struct {
    uac2_format_t format;
    uint32_t sample_rate;   /* PCM / DoP carrier rate */
    uint8_t  bits;
    uint8_t  channels;
    uac2_dsd_rate_t dsd_rate; /* DSD only */
} uac2_stream_config_t;

#ifdef __cplusplus
}
#endif
