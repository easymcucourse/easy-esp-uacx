/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdatomic.h>
#include "easy_uacx.h"
#include "euacx_driver.h"

#define EUACX_MAX_ALTS 32
#define EUACX_MAX_FEATURES 16
typedef struct {
    uint8_t interface, alt, ep, interval, terminal, clock, bits, subslot, sync;
    uint8_t feedback_ep, feedback_interval;
    uint16_t mps, feedback_mps;
    euacx_rate_list_t rates;
} euacx_alt_t;
typedef struct {
    uint8_t id, source;
    uint32_t mute_read, mute_write, volume_read, volume_write;
} euacx_feature_t;
typedef struct euacx_dev_caps {
    uint8_t ac_interface, num_alts, num_features, first_clock;
    euacx_alt_t alts[EUACX_MAX_ALTS];
    euacx_feature_t features[EUACX_MAX_FEATURES];
    uint8_t source[256], clock[256], clock_controls[256];
    bool clock_fallback;
} euacx_dev_caps_t;

esp_err_t euacx_parse(const uint8_t *data, size_t size, euacx_dev_caps_t *caps);
const euacx_feature_t *euacx_feature(const euacx_dev_caps_t *caps, uint8_t terminal);
uint32_t euacx_service_ticks(const euacx_alt_t *alt);
bool euacx_alt_fits(const euacx_alt_t *alt, euacx_speed_t speed, uint32_t rate, uint8_t bits);
const euacx_alt_t *euacx_select(const euacx_dev_caps_t *caps, euacx_speed_t speed,
                               const euacx_stream_config_t *request);
const euacx_alt_t *euacx_select_verified(const euacx_dev_caps_t *caps, euacx_speed_t speed,
                                        const euacx_stream_config_t *request, const euacx_driver_t *driver,
                                        bool verified);
void euacx_build_info(const euacx_dev_caps_t *caps, const euacx_driver_t *driver,
                      bool verified, euacx_info_t *info);
esp_err_t euacx_parse_rates(const uint8_t *data, size_t size, euacx_rate_list_t *rates);
void euacx_standard_rates(euacx_rate_list_t *rates);
int16_t euacx_volume_snap(int16_t value, int16_t min, int16_t max, int16_t step);
bool euacx_feedback_rate(const uint8_t *data, size_t size, euacx_speed_t speed,
                          uint32_t nominal, uint32_t *rate_q16);
size_t euacx_pcm_convert(const uint8_t *src, size_t samples, uint8_t bits,
                         uint8_t subslot, uint8_t *dst);

/* A power-of-two SPSC ring makes unsigned counter wrap safe on 32-bit MCUs. */
typedef struct {
    uint8_t *data;
    uint32_t size;
    _Atomic uint32_t read, write;
} euacx_ring_t;
uint32_t euacx_ring_used(const euacx_ring_t *ring);
uint32_t euacx_ring_space(const euacx_ring_t *ring);
size_t euacx_ring_put(euacx_ring_t *ring, const void *data, size_t size);
size_t euacx_ring_take(euacx_ring_t *ring, void *data, size_t size);

typedef enum { EUACX_OP_INFO, EUACX_OP_OPEN, EUACX_OP_WRITE, EUACX_OP_CLOSE,
               EUACX_OP_ABORT, EUACX_OP_CONTROL } euacx_operation_t;
esp_err_t euacx_state_check(euacx_state_t state, euacx_operation_t op,
                            bool pull, bool owner, bool feed, bool aborted);
