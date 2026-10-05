/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "easy_uacx.h"

typedef struct {
    uint8_t bits, subslot;
    euacx_rate_list_t rates;
} euacx_verified_pcm_t;

typedef struct {
    const euacx_verified_pcm_t *fs, *hs;
    uint8_t num_fs, num_hs;
    bool has_volume;
    int16_t volume_db256;
    const char *verified;
} euacx_verified_caps_t;

typedef struct euacx_dev euacx_dev_t;
typedef struct euacx_dev_caps euacx_dev_caps_t;
typedef struct euacx_stream_state euacx_stream_state_t;
typedef struct euacx_driver {
    const char *name;
    uint16_t vid, pid; /* pid 0 matches every product of vid */
    const euacx_verified_caps_t *verified;
    esp_err_t (*attach)(euacx_dev_t *dev, void **ctx);
    void (*detach)(euacx_dev_t *dev, void *ctx);
    esp_err_t (*fixup_caps)(euacx_dev_t *dev, void *ctx, euacx_dev_caps_t *caps);
    esp_err_t (*stream_start)(euacx_dev_t *dev, void *ctx, const euacx_stream_state_t *stream);
    esp_err_t (*stream_stop)(euacx_dev_t *dev, void *ctx);
} euacx_driver_t;

extern const euacx_driver_t euacx_drv_generic;
extern const euacx_driver_t euacx_drv_cx31993;

/* Exact product takes priority over a vendor wildcard, regardless of table order. */
const euacx_driver_t *euacx_driver_match(const euacx_driver_t *const *table,
                                       size_t count, uint16_t vid, uint16_t pid);
const euacx_driver_t *euacx_driver_find(uint16_t vid, uint16_t pid);
esp_err_t euacx_driver_validate(const euacx_driver_t *driver);
