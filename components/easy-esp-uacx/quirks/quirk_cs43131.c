/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "quirk_entries.h"

/* CS43131 dongle (product string "CS43198 HiFi DSP Audio") enumerates on S3 FS as
 * 3302:98c2, UAC2 with async feedback 0x83 (bInterval 4). AS interface 1 alts 1/2/3
 * are PCM16/24/32 and every alt declares a 776-byte ISO OUT MPS on endpoint 0x07,
 * above S3's 600-byte periodic OUT FIFO, so at FS the host pipe MPS is capped at
 * 582 bytes; the device never sees the cap.
 *
 * FS firmware quirks found on S3 2026-11-22:
 * - GET_INTERFACE answers 35 after SET_INTERFACE alt 0, so the alt readback is skipped.
 * - The UAC2 clock accepts and reads back 48/88.2/96 kHz, but only 44.1 kHz plays:
 *   recorded output at other rates is garbled (PCM24/32) or muted (PCM16), the UAC1
 *   endpoint rate request STALLs for every rate but 44.1 kHz, and the feedback
 *   endpoint returns zero-length packets. The FS table therefore lists 44.1 kHz only.
 * - Mute readback fails, so mute is not offered; volume readback works. */
static const euacx_verified_pcm_t fs[] = {
    { .bits = 16, .subslot = 2, .rates = { .num_rates = 1, .rates = { 44100 } } },
    { .bits = 24, .subslot = 3, .rates = { .num_rates = 1, .rates = { 44100 } } },
    { .bits = 32, .subslot = 4, .rates = { .num_rates = 1, .rates = { 44100 } } },
};
static const euacx_verified_caps_t caps_fs = {
    .fs = fs, .num_fs = 3,
    .has_volume = true, .volume_db256 = -2688, /* -10.5 dB, as for the other headphone dongles */
    .verified = "2026-11-22 CS43131 3302:98c2 FS public API + recorded output; S3 v0.2; IDF5.5.1 USB1.4.1",
};
static const euacx_driver_params_t params = { .fs_out_mps = 582 };

const euacx_driver_t euacx_quirk_cs43131 = {
    .name = "cs43131", .vid = 0x3302, .pid = 0x98c2,
    .flags = EUACX_DRV_FS_OUT_MPS | EUACX_DRV_ALT_NO_READBACK, .params = &params,
    .verified = &caps_fs,
};
