/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "quirk_entries.h"

/* ES9039Q2M DAC board (product string "SXW-MDL7601-INTCLK_A2") enumerates on P4 HS
 * as 2fc6:f800, UAC2 with async feedback 0x81 every 8 microframes (bInterval 4).
 * AS interface 1 alts 1/2/3 are PCM16/24/32 (alt 4 RAW) and every alt declares a
 * 776-byte ISO OUT MPS on endpoint 0x01, so P4 needs
 * CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT (set in examples/p4/sdkconfig.defaults).
 * Feature Unit 3 has master mute/volume and per-channel volume; mute and volume
 * readback both pass. Public API push/pull matrix passed 48/48 on P4 2026-11-22 and
 * the recorded output showed the correct melody at every rate and bit depth. */
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
    .has_volume = true, .volume_db256 = -2688, /* -10.5 dB, as for the other headphone-capable DACs */
    .verified = "2026-11-22 ES9039Q2M 2fc6:f800 HS public API + recorded output; P4 v1.3; IDF5.5.1 USB1.4.1 PTX bias",
};

const euacx_driver_t euacx_quirk_es9039 = {
    .name = "es9039q2m", .vid = 0x2fc6, .pid = 0xf800,
    .verified = &caps_hs,
};
