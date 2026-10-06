/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "easy_uacx.h"

/* Device behavior flags, independently defined by this project. */
enum {
    EUACX_DRV_VOL_NO_READBACK = 1u << 0,
    EUACX_DRV_VOL_MIN_IS_MUTE = 1u << 1,
    EUACX_DRV_VOL_RANGE = 1u << 2,
    EUACX_DRV_NO_HW_VOLUME = 1u << 3,
    EUACX_DRV_CTL_DELAY = 1u << 4,
    EUACX_DRV_RATE_NO_READBACK = 1u << 5,
    EUACX_DRV_ALT_BEFORE_RATE = 1u << 6,
    EUACX_DRV_IFACE_DELAY = 1u << 7,
};
typedef struct {
    uint16_t ctl_delay_us, iface_delay_ms;
    int16_t vol_min, vol_max, vol_res;
    uint16_t bcd_min, bcd_max; /* 0/0 accepts any firmware revision */
} euacx_driver_params_t;

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
    uint32_t flags;
    const euacx_driver_params_t *params;
    bool reported; /* public report, without project hardware verification */
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
                                       size_t count, uint16_t vid, uint16_t pid, uint16_t bcd);
const euacx_driver_t *euacx_driver_find(uint16_t vid, uint16_t pid, uint16_t bcd);
esp_err_t euacx_driver_validate(const euacx_driver_t *driver);
