/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "euacx_driver.h"

/* FS: public API push/pull matrix passed 2026-10-05; listener confirmed
 * startup pop eliminated after muted priming. 32-bit MPS768 exceeds S3's
 * default periodic FIFO limit and is intentionally not registered.
 * HS: all 48 public API push/pull entries passed 2026-10-05. UR22C input
 * 1/2 recordings confirmed removal of 384 kHz generator starvation noise. */
static const euacx_verified_pcm_t fs[] = {
    { .bits = 16, .subslot = 2,
      .rates = { .num_rates = 6, .rates = { 8000, 16000, 32000, 44100, 48000, 96000 } } },
    { .bits = 24, .subslot = 3,
      .rates = { .num_rates = 6, .rates = { 8000, 16000, 32000, 44100, 48000, 96000 } } },
};
static const euacx_verified_pcm_t hs[] = {
    { .bits = 16, .subslot = 2,
      .rates = { .num_rates = 8, .rates = { 8000, 16000, 32000, 44100, 48000, 96000, 192000, 384000 } } },
    { .bits = 24, .subslot = 3,
      .rates = { .num_rates = 8, .rates = { 8000, 16000, 32000, 44100, 48000, 96000, 192000, 384000 } } },
    { .bits = 32, .subslot = 4,
      .rates = { .num_rates = 8, .rates = { 8000, 16000, 32000, 44100, 48000, 96000, 192000, 384000 } } },
};
static const euacx_verified_caps_t caps = {
    .fs = fs, .hs = hs, .num_fs = 2, .num_hs = 3,
    .has_volume = true, .volume_db256 = -2688, /* -10.5 dB */
    .verified = "2026-10-05 public API; HS recorded on UR22C input 1/2; S3 v0.2 / P4 v1.3; IDF5.5.1 USB1.4.1",
};

const euacx_driver_t euacx_drv_cx31993 = {
    .name = "cx31993", .vid = 0x06cb, .pid = 0x1594, .verified = &caps,
};
