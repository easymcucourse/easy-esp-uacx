/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "quirk_entries.h"

/* ES9318 (Savitech SA9023) enumerates as two products:
 * FS 262a:0002 is UAC1 with discrete 32-96 kHz rates; alt must be set before the
 * endpoint sample-rate request.
 * HS 262a:0001 is UAC2 with async feedback every 8 microframes (bInterval 4).
 * Public API push/pull matrix passed 48/48 on P4 2026-11-22. Mute SET is accepted
 * but GET always reads 0, so the generic readback probe disables mute. */
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

static const euacx_verified_pcm_t hs[] = {
    { .bits = 16, .subslot = 2,
      .rates = { .num_rates = 8, .rates = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 } } },
    { .bits = 24, .subslot = 3,
      .rates = { .num_rates = 8, .rates = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 } } },
    { .bits = 32, .subslot = 4,
      .rates = { .num_rates = 8, .rates = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000 } } },
};
static const euacx_verified_caps_t caps_hs = {
    .hs = hs, .num_hs = 3,
    .has_volume = true, .volume_db256 = 0,
    .verified = "2026-11-22 ES9318 (Savitech SA9023) HS public API; P4 v1.3; IDF5.5.1 USB1.4.1",
};

const euacx_driver_t euacx_quirk_es9318 = {
    .name = "es9318", .vid = 0x262a, .pid = 0x0002,
    .flags = EUACX_DRV_ALT_BEFORE_RATE,
    .verified = &caps,
};

const euacx_driver_t euacx_quirk_es9318_hs = {
    .name = "es9318", .vid = 0x262a, .pid = 0x0001,
    .verified = &caps_hs,
};
