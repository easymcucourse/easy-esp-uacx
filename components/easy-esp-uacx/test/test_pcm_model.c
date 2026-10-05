/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include <limits.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "euacx_model.h"
#include "fixtures/cx31993_fs.h"

static euacx_dev_caps_t model(void)
{
    euacx_dev_caps_t d = {.num_alts = 3};
    d.alts[0] = (euacx_alt_t){.alt = 1, .bits = 32, .subslot = 4, .mps = 1023, .interval = 1,
        .rates = {.num_rates = 3, .rates = {44100,48000,192000}}};
    d.alts[1] = (euacx_alt_t){.alt = 2, .bits = 24, .subslot = 3, .mps = 1023, .interval = 1,
        .rates = {.num_rates = 3, .rates = {44100,48000,96000}}};
    d.alts[2] = (euacx_alt_t){.alt = 3, .bits = 16, .subslot = 2, .mps = 1023, .interval = 1,
        .rates = {.num_rates = 1, .rates = {48000}}};
    return d;
}
TEST_CASE("parser: real CX31993 FS descriptor resolves playback clock and Feature Unit", "[euacx][parser]")
{
    euacx_dev_caps_t d;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_parse(cx31993_fs_config, sizeof(cx31993_fs_config), &d));
    TEST_ASSERT_EQUAL_UINT8(3, d.num_alts); TEST_ASSERT_EQUAL_UINT8(0, d.ac_interface);
    TEST_ASSERT_FALSE(d.clock_fallback);
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL_UINT8(9, d.alts[i].clock);
        TEST_ASSERT_EQUAL_UINT8(1, d.alts[i].interface);
        TEST_ASSERT_EQUAL_UINT8(i + 1, d.alts[i].alt);
        TEST_ASSERT_EQUAL_UINT8(16 + i * 8, d.alts[i].bits);
        TEST_ASSERT_EQUAL_UINT8(2 + i, d.alts[i].subslot);
    }
    const euacx_feature_t *f = euacx_feature(&d, 1);
    TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_EQUAL_UINT8(2, f->id);
    TEST_ASSERT_EQUAL_UINT32(1, f->mute_write); TEST_ASSERT_EQUAL_UINT32(6, f->volume_write);
    f = euacx_feature(&d, 6); TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_EQUAL_UINT8(5, f->id);
    uint8_t broken[sizeof(cx31993_fs_config)]; memcpy(broken, cx31993_fs_config, sizeof(broken));
    broken[9] = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, euacx_parse(broken, sizeof(broken), &d));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_parse(cx31993_fs_config, sizeof(cx31993_fs_config) - 1, &d));
}
TEST_CASE("PCM selector: exact bits, narrowest conversion, bandwidth and DSD reject", "[euacx][selector]")
{
    euacx_dev_caps_t d = model();
    euacx_stream_config_t req = {.format = EUACX_FORMAT_PCM, .channels = 2, .bits = 16, .sample_rate = 48000};
    TEST_ASSERT_EQUAL_UINT8(3, euacx_select(&d, EUACX_SPEED_FS, &req)->alt);
    req.sample_rate = 44100;
    TEST_ASSERT_EQUAL_UINT8(2, euacx_select(&d, EUACX_SPEED_FS, &req)->alt);
    req.bits = 32; req.sample_rate = 192000;
    TEST_ASSERT_NULL(euacx_select(&d, EUACX_SPEED_FS, &req));
    TEST_ASSERT_EQUAL_UINT8(1, euacx_select(&d, EUACX_SPEED_HS, &req)->alt);
    req.format = EUACX_FORMAT_DSD; TEST_ASSERT_NULL(euacx_select(&d, EUACX_SPEED_HS, &req));
    req.format = EUACX_FORMAT_DOP; TEST_ASSERT_NULL(euacx_select(&d, EUACX_SPEED_HS, &req));
    req.format = EUACX_FORMAT_PCM; req.channels = 1; TEST_ASSERT_NULL(euacx_select(&d, EUACX_SPEED_HS, &req));
}
TEST_CASE("PCM caps: sorted per-depth lists and verified mismatch exclusion", "[euacx][caps]")
{
    euacx_dev_caps_t d = model();
    euacx_info_t info = {.speed = EUACX_SPEED_FS};
    euacx_build_info(&d, &euacx_drv_generic, false, &info);
    TEST_ASSERT_FALSE(info.verified); TEST_ASSERT_EQUAL_UINT8(3, info.pcm[0].num_rates);
    TEST_ASSERT_EQUAL_UINT32(44100, info.pcm[0].rates[0]);
    TEST_ASSERT_EQUAL_UINT8(2, info.pcm[2].num_rates); TEST_ASSERT_EQUAL_UINT8(0, info.dsd.num_rates);
    const euacx_verified_pcm_t pcm = {.bits = 24, .subslot = 3, .rates = {.num_rates = 3, .rates = {44100,48000,192000}}};
    const euacx_verified_caps_t verified = {.fs = &pcm, .num_fs = 1};
    const euacx_driver_t driver = {.name = "fixture", .verified = &verified};
    euacx_build_info(&d, &driver, true, &info);
    TEST_ASSERT_TRUE(info.verified); TEST_ASSERT_EQUAL_UINT8(0, info.pcm[0].num_rates);
    TEST_ASSERT_EQUAL_UINT8(2, info.pcm[1].num_rates); TEST_ASSERT_EQUAL_UINT8(0, info.pcm[2].num_rates);
}
TEST_CASE("PCM bytes: signed extrema and 24-bit four-byte padding", "[euacx][pcm]")
{
    const uint8_t in16[] = {0x00,0x80,0xff,0x7f};
    uint8_t out[8];
    const uint8_t exp24[] = {0,0,0x80,0,0xff,0x7f};
    TEST_ASSERT_EQUAL(6, euacx_pcm_convert(in16, 2, 16, 3, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp24, out, 6);
    const uint8_t exp32[] = {0,0,0,0x80,0,0,0xff,0x7f};
    TEST_ASSERT_EQUAL(8, euacx_pcm_convert(in16, 2, 16, 4, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(exp32, out, 8);
    const uint8_t in24[] = {0x11,0x22,0x80,0xff,0xff,0x7f};
    const uint8_t pad[] = {0,0x11,0x22,0x80,0,0xff,0xff,0x7f};
    TEST_ASSERT_EQUAL(8, euacx_pcm_convert(in24, 2, 24, 4, out)); TEST_ASSERT_EQUAL_UINT8_ARRAY(pad, out, 8);
    TEST_ASSERT_EQUAL(0, euacx_pcm_convert(in24, 2, 24, 2, out));
}
TEST_CASE("verified selector: open uses the tested subslot even when a narrower alt exists", "[euacx][caps]")
{
    euacx_dev_caps_t d = model();
    const euacx_verified_pcm_t pcm = {.bits = 16, .subslot = 3, .rates = {.num_rates = 1, .rates = {48000}}};
    const euacx_verified_caps_t verified = {.fs = &pcm, .num_fs = 1};
    const euacx_driver_t driver = {.name = "fixture", .verified = &verified};
    euacx_stream_config_t req = {.format = EUACX_FORMAT_PCM, .channels = 2, .bits = 16, .sample_rate = 48000};
    TEST_ASSERT_EQUAL_UINT8(2, euacx_select_verified(&d, EUACX_SPEED_FS, &req, &driver, true)->alt);
    TEST_ASSERT_EQUAL_UINT8(3, euacx_select_verified(&d, EUACX_SPEED_FS, &req, &driver, false)->alt);
    d.alts[1].subslot = 4;
    TEST_ASSERT_NULL(euacx_select_verified(&d, EUACX_SPEED_FS, &req, &driver, true));
    euacx_info_t info = {.speed = EUACX_SPEED_FS};
    euacx_build_info(&d, &driver, true, &info);
    TEST_ASSERT_EQUAL_UINT8(0, info.pcm[0].num_rates);
}
TEST_CASE("feedback bandwidth: spare frame and valid feedback endpoint are required", "[euacx][feedback]")
{
    euacx_dev_caps_t d = model();
    euacx_alt_t *a = &d.alts[1];
    a->feedback_ep = 0x81; a->feedback_mps = 3; a->feedback_interval = 1;
    a->mps = 288;
    TEST_ASSERT_FALSE(euacx_alt_fits(a, EUACX_SPEED_FS, 48000, 24));
    a->mps = 294;
    TEST_ASSERT_TRUE(euacx_alt_fits(a, EUACX_SPEED_FS, 48000, 24));
    a->feedback_interval = 0;
    TEST_ASSERT_FALSE(euacx_alt_fits(a, EUACX_SPEED_FS, 48000, 24));
    a->feedback_interval = 1; a->feedback_mps = 5;
    TEST_ASSERT_FALSE(euacx_alt_fits(a, EUACX_SPEED_FS, 48000, 24));
    a->feedback_mps = 3;
    TEST_ASSERT_FALSE(euacx_alt_fits(a, EUACX_SPEED_HS, 48000, 24));
    a->feedback_mps = 4;
    TEST_ASSERT_TRUE(euacx_alt_fits(a, EUACX_SPEED_HS, 48000, 24));
}
TEST_CASE("SPSC ring: wrap across UINT32_MAX keeps ordering", "[euacx][ring]")
{
    uint8_t storage[8], dst[8];
    euacx_ring_t r = {.data = storage, .size = 8};
    atomic_store(&r.read, UINT32_MAX - 3); atomic_store(&r.write, UINT32_MAX - 3);
    const uint8_t data[] = {1,2,3,4,5,6,7,8};
    TEST_ASSERT_EQUAL(8, euacx_ring_put(&r, data, 8));
    TEST_ASSERT_EQUAL(8, euacx_ring_used(&r)); TEST_ASSERT_EQUAL(0, euacx_ring_space(&r));
    TEST_ASSERT_EQUAL(8, euacx_ring_take(&r, dst, 8)); TEST_ASSERT_EQUAL_UINT8_ARRAY(data, dst, 8);
    TEST_ASSERT_EQUAL(0, euacx_ring_used(&r));
}
TEST_CASE("feedback: FS 10.14, UAC2 16.16 and malformed/outlier rejection", "[euacx][feedback]")
{
    uint32_t value;
    const uint8_t fs[] = {0,0,12}; /* 48 samples/frame, 10.14 */
    TEST_ASSERT_TRUE(euacx_feedback_rate(fs, 3, EUACX_SPEED_FS, 48000, &value));
    TEST_ASSERT_EQUAL_UINT32(48u << 16, value);
    const uint8_t hs[] = {0,0,6,0}; /* 6 samples/microframe, 16.16 */
    TEST_ASSERT_TRUE(euacx_feedback_rate(hs, 4, EUACX_SPEED_HS, 48000, &value));
    TEST_ASSERT_EQUAL_UINT32(6u << 16, value);
    TEST_ASSERT_FALSE(euacx_feedback_rate(fs, 3, EUACX_SPEED_HS, 48000, &value));
    TEST_ASSERT_FALSE(euacx_feedback_rate(hs, 4, EUACX_SPEED_HS, 96000, &value));
}
TEST_CASE("rates: discrete and continuous RANGE replies are sorted and checked", "[euacx][rates]")
{
    const uint8_t data[] = {2,0, 0x80,0xbb,0,0, 0x80,0xbb,0,0, 0,0,0,0,
        0x44,0xac,0,0, 0x44,0xac,0,0, 0,0,0,0};
    euacx_rate_list_t rates;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_parse_rates(data, sizeof(data), &rates));
    TEST_ASSERT_EQUAL_UINT8(2, rates.num_rates); TEST_ASSERT_EQUAL_UINT32(44100, rates.rates[0]);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, euacx_parse_rates(data, sizeof(data) - 1, &rates));
    const uint8_t continuous[] = {1,0, 0,0x7d,0,0, 0x00,0x77,1,0, 0x80,0x3e,0,0}; /* 32..96k step 16k */
    TEST_ASSERT_EQUAL(ESP_OK, euacx_parse_rates(continuous, sizeof(continuous), &rates));
    TEST_ASSERT_EQUAL_UINT8(4, rates.num_rates);
    const uint32_t expected[] = {32000,48000,64000,96000};
    TEST_ASSERT_EQUAL_UINT32_ARRAY(expected, rates.rates, 4);
}
TEST_CASE("volume: clamp and snap do not overflow signed 16-bit limits", "[euacx][volume]")
{
    TEST_ASSERT_EQUAL_INT16(-2688, euacx_volume_snap(-2677, -32768, 0, 128));
    TEST_ASSERT_EQUAL_INT16(0, euacx_volume_snap(1000, -32768, 0, 128));
    TEST_ASSERT_EQUAL_INT16(-32768, euacx_volume_snap(-32768, -32768, 32767, 256));
}

TEST_CASE("state: every state/API combination, ownership and abort", "[euacx][state]")
{
    for (int state = EUACX_STATE_DISCONNECTED; state <= EUACX_STATE_STREAMING; ++state) {
        bool connected = state == EUACX_STATE_CONNECTED || state == EUACX_STATE_STREAMING;
        TEST_ASSERT_EQUAL(connected ? ESP_OK : ESP_ERR_INVALID_STATE, euacx_state_check(state, EUACX_OP_INFO, false, true, false, false));
        TEST_ASSERT_EQUAL(connected ? ESP_OK : ESP_ERR_INVALID_STATE, euacx_state_check(state, EUACX_OP_CONTROL, false, true, false, false));
        TEST_ASSERT_EQUAL(state == EUACX_STATE_CONNECTED ? ESP_OK : ESP_ERR_INVALID_STATE, euacx_state_check(state, EUACX_OP_OPEN, false, true, false, false));
        TEST_ASSERT_EQUAL(state == EUACX_STATE_STREAMING ? ESP_OK : ESP_ERR_INVALID_STATE, euacx_state_check(state, EUACX_OP_WRITE, false, true, false, false));
        TEST_ASSERT_EQUAL(ESP_OK, euacx_state_check(state, EUACX_OP_CLOSE, false, true, false, false));
        TEST_ASSERT_EQUAL(ESP_OK, euacx_state_check(state, EUACX_OP_ABORT, false, false, false, false));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_state_check(EUACX_STATE_STREAMING, EUACX_OP_WRITE, true, true, false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_state_check(EUACX_STATE_STREAMING, EUACX_OP_WRITE, false, false, false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_state_check(EUACX_STATE_STREAMING, EUACX_OP_WRITE, false, true, false, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_state_check(EUACX_STATE_STREAMING, EUACX_OP_CLOSE, false, false, false, false));
    TEST_ASSERT_EQUAL(ESP_OK, euacx_state_check(EUACX_STATE_STREAMING, EUACX_OP_CLOSE, true, false, false, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_state_check(EUACX_STATE_STREAMING, EUACX_OP_CLOSE, true, false, true, false));
}
