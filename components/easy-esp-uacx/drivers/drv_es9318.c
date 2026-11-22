/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "euacx_driver.h"

/* FS: 16-bit and 24-bit stereo PCM supported on ES9318 (Savitech SA9023).
 * Discrete rates: 32000, 44100, 48000, 88200, 96000 Hz. */
static const euacx_verified_pcm_t fs[] = {
    { .bits = 16, .subslot = 2,
      .rates = { .num_rates = 5, .rates = { 32000, 44100, 48000, 88200, 96000 } } },
    { .bits = 24, .subslot = 3,
      .rates = { .num_rates = 5, .rates = { 32000, 44100, 48000, 88200, 96000 } } },
};
static const euacx_verified_caps_t caps = {
    .fs = fs, .num_fs = 2,
    .has_volume = true, .volume_db256 = 0,
    .verified = "2026-11-22 ES9318 (Savitech SA9023) public API; S3 v0.2; IDF5.5.1 USB1.4.1",
};

const euacx_driver_t euacx_drv_es9318 = {
    .name = "es9318", .vid = 0x262a, .pid = 0x0002,
    .flags = EUACX_DRV_ALT_BEFORE_RATE,
    .verified = &caps,
};
