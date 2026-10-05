/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "esp_system.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "uac2_internal.h"
#include "euacx_driver.h"
#include "sdkconfig.h"

/* ---------- fixtures ---------- */
/* Generic stereo DAC: 24-bit PCM 44.1k-384k (subslot 3, MPS 1024 FS-style) + 32-bit raw (DSD native). */
static uac2_stream_cap_t s_caps[] = {
    { .interface_num = 1, .alt_setting = 1, .ep_addr = 0x01, .max_packet_size = 1023, .channels = 2,
      .subslot_size = 3, .bit_resolution = 24, .rate_min = 44100, .rate_max = 192000, .flags = UAC2_CAP_FLAG_PCM },
    { .interface_num = 1, .alt_setting = 2, .ep_addr = 0x01, .max_packet_size = 1024, .channels = 2,
      .subslot_size = 4, .bit_resolution = 32, .rate_min = 44100, .rate_max = 384000, .flags = UAC2_CAP_FLAG_RAW_DATA },
    { .interface_num = 1, .alt_setting = 3, .ep_addr = 0x01, .max_packet_size = 3072, .channels = 2,
      .subslot_size = 3, .bit_resolution = 24, .rate_min = 44100, .rate_max = 384000, .flags = UAC2_CAP_FLAG_PCM },
};
static const uac2_host_hw_caps_t HOST_FS = { .max_speed = 0, .max_iso_payload = 1023, .high_bandwidth_iso = false };
static const uac2_host_hw_caps_t HOST_HS = { .max_speed = 1, .max_iso_payload = 3072, .high_bandwidth_iso = true };

static uac2_device_caps_t dac(uint32_t flags, int ncaps)
{
    uac2_device_caps_t d = { .num_playback_caps = (uint8_t)ncaps, .playback_caps = s_caps, .feature_flags = flags };
    return d;
}

/* ---------- packet size ---------- */
TEST_CASE("packet_bytes: 48k stereo 24bit on FS = 288", "[uac2][packet]")
{
    TEST_ASSERT_EQUAL_UINT32(288, uac2_packet_bytes(48000, 2, 3, 1000));
}
TEST_CASE("packet_bytes: rounds up", "[uac2][packet]")
{
    TEST_ASSERT_EQUAL_UINT32(177, uac2_packet_bytes(44100, 2, 2, 1000));  /* 176.4 */
    TEST_ASSERT_EQUAL_UINT32(0, uac2_packet_bytes(48000, 2, 3, 0));
}
TEST_CASE("packet_bytes: HS microframe is 1/8", "[uac2][packet]")
{
    TEST_ASSERT_EQUAL_UINT32(36, uac2_packet_bytes(48000, 2, 3, 8000));
}

/* ---------- DSD rates ---------- */
TEST_CASE("dsd rates", "[uac2][dsd]")
{
    TEST_ASSERT_EQUAL_UINT32(176400, uac2_dsd_dop_rate(UAC2_DSD64));
    TEST_ASSERT_EQUAL_UINT32(352800, uac2_dsd_dop_rate(UAC2_DSD128));
    TEST_ASSERT_EQUAL_UINT32(88200, uac2_dsd_native_rate(UAC2_DSD64));
}

/* ---------- selector ---------- */
TEST_CASE("selector: PCM 96k/24 on FS ok", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(0, 3);
    uac2_stream_config_t req = { .format = UAC2_FORMAT_PCM, .sample_rate = 96000, .bits = 24, .channels = 2 }, out;
    const uac2_stream_cap_t *c = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uac2_find_best_mode(&d, &HOST_FS, &req, &out, &c));
    TEST_ASSERT_EQUAL_UINT8(1, c->alt_setting);
}
TEST_CASE("selector: PCM 192k/24 stereo rejected on FS (bandwidth)", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(0, 1);  /* only alt1: 192k*2*3=1152 > 1023 */
    uac2_stream_config_t req = { .format = UAC2_FORMAT_PCM, .sample_rate = 192000, .bits = 24, .channels = 2 }, out;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, uac2_find_best_mode(&d, &HOST_FS, &req, &out, NULL));
}
TEST_CASE("selector: PCM 384k/24 ok on HS with high-bandwidth alt", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(0, 3);
    uac2_stream_config_t req = { .format = UAC2_FORMAT_PCM, .sample_rate = 384000, .bits = 24, .channels = 2 }, out;
    const uac2_stream_cap_t *c = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uac2_find_best_mode(&d, &HOST_HS, &req, &out, &c));
    TEST_ASSERT_EQUAL_UINT8(3, c->alt_setting);
}
TEST_CASE("selector: unsupported rate rejected", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(0, 1);
    uac2_stream_config_t req = { .format = UAC2_FORMAT_PCM, .sample_rate = 8000, .bits = 24, .channels = 2 }, out;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, uac2_find_best_mode(&d, &HOST_FS, &req, &out, NULL));
}
TEST_CASE("selector: DSD64 native preferred on FS", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(UAC2_QUIRK_NATIVE_DSD, 3);
    uac2_stream_config_t req = { .format = UAC2_FORMAT_DSD_DOP, .channels = 2, .dsd_rate = UAC2_DSD64 }, out;
    TEST_ASSERT_EQUAL(ESP_OK, uac2_find_best_mode(&d, &HOST_FS, &req, &out, NULL));
    TEST_ASSERT_EQUAL(UAC2_FORMAT_DSD_NATIVE, out.format);  /* 88200*2*4=705.6KB/s fits */
}
TEST_CASE("selector: DoP64 rejected on FS (1059 > 1023), ok on HS", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(0, 3);  /* no native quirk */
    uac2_stream_config_t req = { .format = UAC2_FORMAT_DSD_DOP, .channels = 2, .dsd_rate = UAC2_DSD64 }, out;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, uac2_find_best_mode(&d, &HOST_FS, &req, &out, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, uac2_find_best_mode(&d, &HOST_HS, &req, &out, NULL));
    TEST_ASSERT_EQUAL(UAC2_FORMAT_DSD_DOP, out.format);
    TEST_ASSERT_EQUAL_UINT32(176400, out.sample_rate);
}
TEST_CASE("selector: native needs quirk flag", "[uac2][selector]")
{
    uac2_device_caps_t d = dac(0, 2);  /* raw cap but no quirk, DoP cap too slow on FS */
    uac2_stream_config_t req = { .format = UAC2_FORMAT_DSD_DOP, .channels = 2, .dsd_rate = UAC2_DSD64 }, out;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, uac2_find_best_mode(&d, &HOST_FS, &req, &out, NULL));
}
TEST_CASE("selector: invalid args", "[uac2][selector]")
{
    uac2_stream_config_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, uac2_find_best_mode(NULL, &HOST_FS, NULL, &out, NULL));
}

/* ---------- DoP ---------- */
TEST_CASE("dop: marker alternates, byte order", "[uac2][dop]")
{
    dop_context_t ctx = { .marker = 0x05 };
    const uint8_t in[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };  /* 2 frames stereo */
    uint8_t out[12];
    TEST_ASSERT_EQUAL(12, uac2_dop_pack(&ctx, in, sizeof(in), 2, out, sizeof(out)));
    const uint8_t exp[12] = { 0x22, 0x11, 0x05, 0x44, 0x33, 0x05, 0x66, 0x55, 0xFA, 0x88, 0x77, 0xFA };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, out, 12);
    TEST_ASSERT_EQUAL_UINT8(0x05, ctx.marker);  /* continues the sequence */
}
TEST_CASE("dop: output size limits frames", "[uac2][dop]")
{
    dop_context_t ctx = { .marker = 0x05 };
    uint8_t in[16] = { 0 }, out[7];
    TEST_ASSERT_EQUAL(6, uac2_dop_pack(&ctx, in, sizeof(in), 2, out, sizeof(out)));
}

/* ---------- ring ---------- */
TEST_CASE("ring: write/read/wrap", "[uac2][ring]")
{
    audio_ring_t r;
    TEST_ASSERT_EQUAL(ESP_OK, audio_ring_init(&r, 8));
    uint8_t a[6] = { 1, 2, 3, 4, 5, 6 }, b[6];
    TEST_ASSERT_EQUAL(6, audio_ring_write(&r, a, 6));
    TEST_ASSERT_EQUAL(4, audio_ring_read(&r, b, 4));
    TEST_ASSERT_EQUAL(6, audio_ring_write(&r, a, 6));  /* wraps */
    TEST_ASSERT_EQUAL(0, audio_ring_free(&r));
    TEST_ASSERT_EQUAL(0, audio_ring_write(&r, a, 2));  /* full */
    uint8_t all[8];
    TEST_ASSERT_EQUAL(8, audio_ring_read(&r, all, 8));
    const uint8_t exp[8] = { 5, 6, 1, 2, 3, 4, 5, 6 };
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp, all, 8);
    audio_ring_deinit(&r);
    TEST_ASSERT_NULL(r.buffer);
}
TEST_CASE("ring: init invalid", "[uac2][ring]")
{
    audio_ring_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_ring_init(&r, 0));
}
TEST_CASE("ring: no heap leak over init/deinit cycles", "[uac2][ring]")
{
    audio_ring_t r;
    size_t before = esp_get_free_heap_size();
    for (int i = 0; i < 10; i++) { audio_ring_init(&r, 4096); audio_ring_deinit(&r); }
    TEST_ASSERT_INT_WITHIN(64, before, esp_get_free_heap_size());
}

/* ---------- format driver vtable ---------- */
TEST_CASE("drivers: exact product outranks earlier vendor wildcard", "[euacx][drivers]")
{
    const euacx_driver_t vendor = { .name = "vendor", .vid = 0x1234 };
    const euacx_driver_t exact = { .name = "exact", .vid = 0x1234, .pid = 0x5678 };
    const euacx_driver_t *const table[] = { &vendor, &exact };
    TEST_ASSERT_EQUAL_PTR(&exact, euacx_driver_match(table, 2, 0x1234, 0x5678));
    TEST_ASSERT_EQUAL_PTR(&vendor, euacx_driver_match(table, 2, 0x1234, 0x9999));
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_generic, euacx_driver_match(table, 2, 0x9999, 0x5678));
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_generic, euacx_driver_match(NULL, 0, 0x1234, 0x5678));
}

TEST_CASE("drivers: only verified CX31993 USB ID is registered", "[euacx][drivers]")
{
    const euacx_driver_t *d = euacx_driver_find(0x06cb, 0x1594);
#ifdef CONFIG_EUACX_DRV_CX31993
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_cx31993, d);
    TEST_ASSERT_EQUAL(ESP_OK, euacx_driver_validate(d));
    TEST_ASSERT_EQUAL_UINT8(2, d->verified->num_fs);
    TEST_ASSERT_EQUAL_UINT8(16, d->verified->fs[0].bits);
    TEST_ASSERT_EQUAL_UINT8(2, d->verified->fs[0].subslot);
    TEST_ASSERT_EQUAL_UINT8(24, d->verified->fs[1].bits);
    TEST_ASSERT_EQUAL_UINT8(3, d->verified->fs[1].subslot);
    TEST_ASSERT_EQUAL_UINT8(6, d->verified->fs[0].rates.num_rates);
    TEST_ASSERT_EQUAL_UINT8(6, d->verified->fs[1].rates.num_rates);
    TEST_ASSERT_EQUAL_UINT8(3, d->verified->num_hs);
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT8(16 + i * 8, d->verified->hs[i].bits);
        TEST_ASSERT_EQUAL_UINT8(2 + i, d->verified->hs[i].subslot);
        TEST_ASSERT_EQUAL_UINT8(8, d->verified->hs[i].rates.num_rates);
        TEST_ASSERT_EQUAL_UINT32(384000, d->verified->hs[i].rates.rates[7]);
    }
    TEST_ASSERT_EQUAL_INT16(-2688, d->verified->volume_db256);
#else
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_generic, d);
#endif
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_generic, euacx_driver_find(0x0572, 0x1b08));
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_generic, euacx_driver_find(0x0572, 0x1b09));
    TEST_ASSERT_EQUAL_PTR(&euacx_drv_generic, euacx_driver_find(0x20b1, 0x1234));
}

TEST_CASE("drivers: verified tables reject invalid bit depth and rate order", "[euacx][drivers]")
{
    euacx_verified_pcm_t pcm = { .bits = 24, .subslot = 3,
        .rates = { .num_rates = 2, .rates = { 44100, 48000 } } };
    euacx_verified_caps_t caps = { .fs = &pcm, .num_fs = 1 };
    euacx_driver_t d = { .name = "test", .verified = &caps };
    TEST_ASSERT_EQUAL(ESP_OK, euacx_driver_validate(&d));
    pcm.subslot = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&d));
    pcm.subslot = 3;
    pcm.bits = 20;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&d));
    pcm.bits = 24;
    pcm.rates.rates[1] = 44100;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&d));
    pcm.rates.rates[1] = 32000;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&d));
    pcm.rates.num_rates = EUACX_MAX_RATES + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&d));
    caps.fs = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&d));
    TEST_ASSERT_EQUAL(ESP_OK, euacx_driver_validate(&euacx_drv_generic));
}

TEST_CASE("format drivers probe", "[uac2][format]")
{
    uac2_device_caps_t d = dac(UAC2_QUIRK_NATIVE_DSD, 3);
    uac2_stream_config_t cfg = { .format = UAC2_FORMAT_PCM, .sample_rate = 96000, .bits = 24, .channels = 2 };
    TEST_ASSERT_EQUAL(ESP_OK, uac2_pcm_driver.probe(&d, &s_caps[0], &cfg));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, uac2_native_dsd_driver.probe(&d, &s_caps[0], &cfg));
    TEST_ASSERT_EQUAL(ESP_OK, uac2_native_dsd_driver.probe(&d, &s_caps[1], &cfg));
    d.feature_flags = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, uac2_native_dsd_driver.probe(&d, &s_caps[1], &cfg));
}

/* ---------- dac capabilities summary (FS / HS split) ---------- */
TEST_CASE("dac_caps: FS and HS reported separately", "[uac2][caps]")
{
    uac2_dac_caps_t caps;

    /* Generic DAC, no quirk: FS has PCM only; HS adds DoP */
    uac2_device_caps_t d = dac(0, 3);
    TEST_ASSERT_EQUAL(ESP_OK, uac2_get_dac_caps(&d, &caps));
    TEST_ASSERT_TRUE(caps.fs.supports_pcm);
    TEST_ASSERT_EQUAL_UINT32(44100, caps.fs.min_pcm_rate);
    TEST_ASSERT_EQUAL_UINT8(2, caps.fs.max_channels);
    TEST_ASSERT_FALSE(caps.fs.supports_dsd);
    TEST_ASSERT_TRUE(caps.hs.supports_pcm);
    TEST_ASSERT_TRUE(caps.hs.supports_dsd);
    TEST_ASSERT_TRUE(caps.hs.supports_dsd_dop);
    TEST_ASSERT_FALSE(caps.hs.supports_dsd_native);
    TEST_ASSERT_EQUAL_INT(UAC2_DSD128, caps.hs.max_dsd_rate);
    TEST_ASSERT_TRUE(caps.hs.max_pcm_rate > caps.fs.max_pcm_rate || caps.hs.max_pcm_rate == caps.fs.max_pcm_rate);

    /* Native-DSD quirk: DSD works on FS already */
    uac2_device_caps_t x = dac(UAC2_QUIRK_NATIVE_DSD, 3);
    TEST_ASSERT_EQUAL(ESP_OK, uac2_get_dac_caps(&x, &caps));
    TEST_ASSERT_TRUE(caps.fs.supports_dsd_native);
    TEST_ASSERT_TRUE(caps.hs.supports_dsd_native);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, uac2_get_dac_caps(NULL, &caps));
}
