/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "quirk_entries.h"

/* CS43198 dongle (product string "YUNJI43198PRO") enumerates on P4 HS as
 * 2fc6:f0fc, UAC2 with async feedback 0x81 every 8 microframes (bInterval 4).
 * AS interface 1 alts 1/2/3 are PCM16/24/32, and every alt declares a 776-byte
 * ISO OUT MPS. That exceeds the 512-byte periodic TX FIFO of IDF's default
 * BALANCED split, so it needs CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
 * (set in examples/p4/sdkconfig.defaults). Public API push/pull matrix passed
 * 48/48 on P4 2026-11-22; volume and mute readback pass. */
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
    .has_volume = true, .volume_db256 = -2688, /* -10.5 dB, as for the CX31993 headphone dongle */
    .verified = "2026-11-22 CS43198 (YUNJI43198PRO) HS public API; P4 v1.3; IDF5.5.1 USB1.4.1 PTX bias",
};

/* No FS table: on S3 FS the device enumerates with the same ID and descriptor, so
 * every alt exceeds S3's 600-byte periodic OUT FIFO. A 2026-11-22 experiment that
 * capped the host pipe MPS streamed cleanly (clock feedback correct, mute 0) but
 * the dongle stayed silent, also with Windows' alt N -> 0 -> rate -> N order. */

const euacx_driver_t euacx_quirk_cs43198 = {
    .name = "cs43198", .vid = 0x2fc6, .pid = 0xf0fc,
    .verified = &caps_hs,
};
