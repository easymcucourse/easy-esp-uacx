/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "euacx_runtime.h"
#include "fixtures/cx31993_fs.h"

static euacx_dev_t dev;
static euacx_port_t port;
static euacx_driver_t driver;
static euacx_driver_params_t params;
static struct {
    int16_t volume[3], min, max, res;
    bool mute[3], stuck_volume, stuck_mute, fail_get;
    uint16_t ranges;
    uint8_t alt, valid[32], bitmap_size;
    uint32_t rate, returned_rate;
    unsigned fail_at, count, claims, releases;
    struct { uint8_t type, req; uint16_t val, index, size; uint32_t us; } events[512];
} fake;
static euacx_stream_config_t cfg = {.format = EUACX_FORMAT_PCM, .bits = 16, .channels = 2, .sample_rate = 48000};
static void init(uint32_t flags)
{
    memset(&dev, 0, sizeof(dev)); memset(&port, 0, sizeof(port)); memset(&fake, 0, sizeof(fake));
    memset(&params, 0, sizeof(params));
    driver = (euacx_driver_t){.name = "fake", .vid = 1, .pid = 2, .flags = flags, .params = &params};
    TEST_ASSERT_EQUAL(ESP_OK, euacx_parse(cx31993_fs_config, sizeof(cx31993_fs_config), &dev.caps));
    for (unsigned i = 0; i < dev.caps.num_alts; ++i) dev.caps.alts[i].rates = (euacx_rate_list_t){.num_rates = 1, .rates = {48000}};
    dev.driver = &driver; dev.claimed = -1;
    dev.feature = euacx_feature(&dev.caps, dev.caps.alts[0].terminal);
    port.dev = &dev; port.info.speed = EUACX_SPEED_FS;
    fake.min = -10240; fake.max = 0; fake.res = 256; fake.ranges = 1;
    for (unsigned i = 0; i < 3; ++i) fake.volume[i] = -5120 - i * 256;
    fake.bitmap_size = 1; fake.valid[0] = 0xff;
}
void euacx_control_delay(uint32_t us)
{
    TEST_ASSERT_LESS_THAN(512, fake.count);
    fake.events[fake.count].type = 0xfc; fake.events[fake.count++].us = us;
}
esp_err_t euacx_claim_interface(euacx_port_t *p, uint8_t interface, uint8_t alt)
{
    (void)alt; ++fake.claims; p->dev->claimed = interface; return ESP_OK;
}
void euacx_release_interface(euacx_port_t *p)
{
    if (p->dev->claimed >= 0) { ++fake.releases; p->dev->claimed = -1; }
}
static void put16(uint8_t *data, int16_t value) { data[0] = value; data[1] = (uint16_t)value >> 8; }
esp_err_t euacx_control_transfer(euacx_port_t *p, uint8_t type, uint8_t req,
                                 uint16_t val, uint16_t index, void *data, uint16_t size)
{
    (void)p;
    TEST_ASSERT_LESS_THAN(512, fake.count);
    unsigned n = fake.count++;
    fake.events[n].type = type; fake.events[n].req = req;
    fake.events[n].val = val; fake.events[n].index = index; fake.events[n].size = size;
    if (fake.fail_at && fake.count == fake.fail_at) return ESP_FAIL;
    uint8_t *bytes = data;
    if (type == 1 && req == 0x0b) { fake.alt = val; return ESP_OK; }
    if (type == 0x81 && req == 0x0a) { bytes[0] = fake.alt; return ESP_OK; }
    if (index == dev.caps.alts[0].interface && val == 0x0200 && type == 0xa1) {
        bytes[0] = fake.bitmap_size;
        if (size > 1) memcpy(bytes + 1, fake.valid, size - 1);
        return ESP_OK;
    }
    if ((index >> 8) == dev.caps.alts[0].clock && val == 0x0100) {
        if (type == 0x21) fake.rate = bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
        else { uint32_t rate = fake.returned_rate ? fake.returned_rate : fake.rate; for (unsigned i = 0; i < 4; ++i) bytes[i] = rate >> (8 * i); }
        return ESP_OK;
    }
    if ((index >> 8) == dev.caps.alts[0].clock && val == 0x0200) { bytes[0] = 1; return ESP_OK; }
    unsigned ch = val & 0xff;
    TEST_ASSERT_LESS_THAN(3, ch);
    bool volume = (val >> 8) == 2;
    if (req == 2) {
        bytes[0] = fake.ranges; bytes[1] = fake.ranges >> 8;
        put16(bytes + 2, fake.min); put16(bytes + 4, fake.max); put16(bytes + 6, fake.res);
    } else if (type == 0x21) {
        if (volume) fake.volume[ch] = (int16_t)(bytes[0] | (unsigned)bytes[1] << 8);
        else fake.mute[ch] = bytes[0] != 0;
    } else {
        if (fake.fail_get) return ESP_FAIL;
        if (volume) put16(bytes, fake.stuck_volume ? -5120 : fake.volume[ch]);
        else bytes[0] = fake.stuck_mute ? 0 : fake.mute[ch];
    }
    return ESP_OK;
}
static unsigned requests(uint8_t type, uint8_t req, uint16_t val)
{
    unsigned count = 0;
    for (unsigned i = 0; i < fake.count; ++i) if (fake.events[i].type == type && fake.events[i].req == req && fake.events[i].val == val) ++count;
    return count;
}
TEST_CASE("behavior: firmware-specific entry outranks unbounded exact product", "[euacx][drivers]")
{
    euacx_driver_params_t range = {.bcd_min = 0x100, .bcd_max = 0x200};
    euacx_driver_t generic = {.name = "any", .vid = 1, .pid = 2};
    euacx_driver_t firmware = {.name = "firmware", .vid = 1, .pid = 2, .params = &range};
    const euacx_driver_t *table[] = {&generic, &firmware};
    TEST_ASSERT_EQUAL_PTR(&firmware, euacx_driver_match(table, 2, 1, 2, 0x100));
    TEST_ASSERT_EQUAL_PTR(&firmware, euacx_driver_match(table, 2, 1, 2, 0x200));
    TEST_ASSERT_EQUAL_PTR(&generic, euacx_driver_match(table, 2, 1, 2, 0x201));
    firmware.reported = true;
#ifdef CONFIG_EUACX_DRV_REPORTED
    TEST_ASSERT_EQUAL_PTR(&firmware, euacx_driver_match(table, 2, 1, 2, 0x150));
#else
    TEST_ASSERT_EQUAL_PTR(&generic, euacx_driver_match(table, 2, 1, 2, 0x150));
#endif
}
TEST_CASE("behavior: parameters and product specificity validated", "[euacx][drivers]")
{
    init(EUACX_DRV_VOL_RANGE | EUACX_DRV_CTL_DELAY | EUACX_DRV_IFACE_DELAY);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver));
    params = (euacx_driver_params_t){.vol_min = -10240, .vol_max = 0, .vol_res = 256, .ctl_delay_us = 20000, .iface_delay_ms = 200};
    TEST_ASSERT_EQUAL(ESP_OK, euacx_driver_validate(&driver));
    params.ctl_delay_us++; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver)); params.ctl_delay_us--;
    params.iface_delay_ms++; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver)); params.iface_delay_ms--;
    params.vol_res = 257; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver)); params.vol_res = 256;
    params.vol_min = params.vol_max; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver)); params.vol_min = -10240;
    params.bcd_min = 2; params.bcd_max = 1; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver)); params.bcd_min = 0;
    driver.params = NULL; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver)); driver.params = &params;
    driver.pid = 0; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, euacx_driver_validate(&driver));
}
TEST_CASE("behavior: volume probe restores selected channel without changing peers", "[euacx][control]")
{
    init(0); int16_t original[3]; memcpy(original, fake.volume, sizeof(original));
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port));
    TEST_ASSERT_TRUE(port.info.has_volume); TEST_ASSERT_TRUE(port.info.has_mute);
    TEST_ASSERT_EQUAL_INT16_ARRAY(original, fake.volume, 3);
    TEST_ASSERT_EQUAL(2, requests(0x21, 1, 0x0201)); TEST_ASSERT_EQUAL(0, requests(0x21, 1, 0x0202));
}
TEST_CASE("behavior: zero resolution corrected and multiple ranges use first", "[euacx][control]")
{
    init(0); fake.res = 0; fake.ranges = 2;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_TRUE(port.info.has_volume);
    TEST_ASSERT_EQUAL(1, port.info.volume_res); TEST_ASSERT_EQUAL(fake.min, port.info.volume_min);
}
TEST_CASE("behavior: fixed or invalid volume range disabled without losing mute", "[euacx][control]")
{
    init(0); fake.min = fake.max;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_FALSE(port.info.has_volume); TEST_ASSERT_TRUE(port.info.has_mute);
    init(0); fake.res = -1; TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_FALSE(port.info.has_volume);
    init(0); fake.ranges = 0; TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_FALSE(port.info.has_volume);
}
TEST_CASE("behavior: stuck readback disables only corresponding controls", "[euacx][control]")
{
    init(0); fake.stuck_volume = true;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_FALSE(port.info.has_volume); TEST_ASSERT_TRUE(port.info.has_mute);
    init(0); fake.stuck_mute = true;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_TRUE(port.info.has_volume); TEST_ASSERT_FALSE(port.info.has_mute);
}
TEST_CASE("behavior: failed probe read restores volume and enumeration survives", "[euacx][control]")
{
    init(0); int16_t original = fake.volume[1]; fake.fail_at = 4;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_FALSE(port.info.has_volume);
    TEST_ASSERT_EQUAL(original, fake.volume[1]); TEST_ASSERT_TRUE(port.info.has_mute);
}
TEST_CASE("behavior: known broken CUR never read and cache follows successful SET", "[euacx][control]")
{
    init(EUACX_DRV_VOL_NO_READBACK); fake.fail_get = true;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_TRUE(port.info.has_volume); TEST_ASSERT_TRUE(port.info.has_mute);
    int16_t volume = -2688; TEST_ASSERT_EQUAL(ESP_OK, euacx_hw_volume(&port, true, &volume));
    TEST_ASSERT_EQUAL(-2816, volume); unsigned before = fake.count; volume = 0;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_hw_volume(&port, false, &volume)); TEST_ASSERT_EQUAL(-2816, volume);
    bool mute = true; TEST_ASSERT_EQUAL(ESP_OK, euacx_hw_mute(&port, false, &mute)); TEST_ASSERT_FALSE(mute);
    TEST_ASSERT_EQUAL(before, fake.count);
    fake.fail_at = fake.count + 2; volume = -4096;
    TEST_ASSERT_EQUAL(ESP_FAIL, euacx_hw_volume(&port, true, &volume));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_hw_volume(&port, false, &volume));
}
TEST_CASE("behavior: override range bypasses device RANGE and hardware volume can be disabled", "[euacx][control]")
{
    init(EUACX_DRV_VOL_RANGE); params.vol_min = -8192; params.vol_max = 0; params.vol_res = 256;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_TRUE(port.info.has_volume);
    TEST_ASSERT_EQUAL(-8192, port.info.volume_min); TEST_ASSERT_EQUAL(0, requests(0xa1, 2, 0x0201));
    init(EUACX_DRV_NO_HW_VOLUME); TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port));
    TEST_ASSERT_FALSE(port.info.has_volume); TEST_ASSERT_TRUE(port.info.has_mute);
    TEST_ASSERT_EQUAL(0, requests(0xa1, 2, 0x0201));
}
TEST_CASE("behavior: minimum mute hidden from volume range and translated to mute", "[euacx][control]")
{
    init(EUACX_DRV_VOL_MIN_IS_MUTE); fake.volume[1] = fake.min;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_TRUE(port.info.has_volume);
    TEST_ASSERT_EQUAL(fake.min + fake.res, port.info.volume_min);
    int16_t volume = fake.min; unsigned before = requests(0x21, 1, 0x0201);
    TEST_ASSERT_EQUAL(ESP_OK, euacx_hw_volume(&port, true, &volume)); TEST_ASSERT_TRUE(fake.mute[0]);
    TEST_ASSERT_EQUAL(before, requests(0x21, 1, 0x0201));
}
TEST_CASE("behavior: master and single channel selection preserve existing rule", "[euacx][control]")
{
    for (unsigned mask = 1; mask <= 7; ++mask) {
        init(0); euacx_feature_t *f = &dev.caps.features[0];
        f->volume_read = f->volume_write = mask;
        TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port)); TEST_ASSERT_TRUE(port.info.has_volume);
        unsigned ch = mask & 1 ? 0 : mask & 2 ? 1 : 2;
        TEST_ASSERT_EQUAL_UINT16(0x0200 | ch, fake.events[0].val);
        int16_t value = -4096; TEST_ASSERT_EQUAL(ESP_OK, euacx_hw_volume(&port, true, &value));
        for (unsigned c = 0; c < 3; ++c) if (mask & (1u << c)) TEST_ASSERT_EQUAL(value, fake.volume[c]);
    }
}
TEST_CASE("behavior: default and alt-first request ordering and rate readback exception", "[euacx][stream]")
{
    euacx_alt_t selected;
    init(0); TEST_ASSERT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(0, fake.events[0].val); TEST_ASSERT_EQUAL(0x0100, fake.events[2].val);
    TEST_ASSERT_EQUAL(0x21, fake.events[2].type); TEST_ASSERT_EQUAL(1, fake.events[5].val);
    init(EUACX_DRV_ALT_BEFORE_RATE | EUACX_DRV_RATE_NO_READBACK); fake.returned_rate = 123;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(1, fake.events[2].val); TEST_ASSERT_EQUAL(0x0100, fake.events[4].val);
    TEST_ASSERT_EQUAL(0, requests(0xa1, 1, 0x0100));
    init(0); fake.returned_rate = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(-1, dev.claimed); TEST_ASSERT_EQUAL(0, fake.alt);
}
TEST_CASE("behavior: delays follow class requests and streaming SET_INTERFACE", "[euacx][stream]")
{
    init(EUACX_DRV_CTL_DELAY | EUACX_DRV_IFACE_DELAY); params.ctl_delay_us = 500; params.iface_delay_ms = 10;
    euacx_alt_t selected; TEST_ASSERT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
    unsigned ctl = 0, iface = 0;
    for (unsigned i = 0; i < fake.count; ++i) if (fake.events[i].type == 0xfc) {
        if (fake.events[i].us == 500) { ++ctl; TEST_ASSERT_EQUAL(0x20, fake.events[i - 1].type & 0x60); }
        if (fake.events[i].us == 10000) { ++iface; TEST_ASSERT_EQUAL(0x0b, fake.events[i - 1].req); TEST_ASSERT_NOT_EQUAL(0, fake.events[i - 1].val); }
    }
    TEST_ASSERT_EQUAL(3, ctl); TEST_ASSERT_EQUAL(1, iface);
}
TEST_CASE("behavior: smallest usable MPS chosen and bitmap selects next candidate", "[euacx][stream]")
{
    init(0); euacx_alt_t a = dev.caps.alts[0]; dev.caps.num_alts = 3;
    a.as_controls = 4; a.mps = 512; dev.caps.alts[0] = a;
    a.alt = 4; a.mps = 256; dev.caps.alts[1] = a;
    a.alt = 5; a.mps = 128; dev.caps.alts[2] = a;
    fake.valid[0] = 3;
    euacx_alt_t selected; TEST_ASSERT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(1, selected.alt); TEST_ASSERT_EQUAL(512, selected.mps);
    TEST_ASSERT_EQUAL(6, requests(0xa1, 1, 0x0200));
    TEST_ASSERT_EQUAL(2, fake.releases); TEST_ASSERT_EQUAL(3, fake.claims);
    TEST_ASSERT_EQUAL(1, requests(1, 0x0b, 1)); TEST_ASSERT_EQUAL(0, requests(1, 0x0b, 4));
}
TEST_CASE("behavior: all bitmap candidates invalid and malformed responses fail cleanly", "[euacx][stream]")
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        init(mode ? EUACX_DRV_ALT_BEFORE_RATE : 0); fake.valid[0] = 1;
        for (unsigned i = 0; i < dev.caps.num_alts; ++i) dev.caps.alts[i].as_controls = 4;
        euacx_alt_t selected; TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, euacx_prepare_stream(&port, &cfg, &selected));
        TEST_ASSERT_EQUAL(-1, dev.claimed); TEST_ASSERT_EQUAL(0, fake.alt);
        TEST_ASSERT_EQUAL(fake.claims, fake.releases);
    }
    init(0); dev.caps.alts[0].as_controls = 4; fake.bitmap_size = 33;
    euacx_alt_t selected; TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(-1, dev.claimed);
}
TEST_CASE("behavior: undeclared alt control makes no bitmap request and flags exposed", "[euacx][stream]")
{
    init(EUACX_DRV_RATE_NO_READBACK); euacx_alt_t selected;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
    for (unsigned i = 0; i < fake.count; ++i) TEST_ASSERT_FALSE(fake.events[i].type == 0xa1 && fake.events[i].index == selected.interface);
    euacx_build_info(&dev.caps, &driver, false, &port.info);
    TEST_ASSERT_EQUAL(EUACX_DRV_RATE_NO_READBACK, port.info.driver_flags);
}
TEST_CASE("behavior: UAC1 descriptor ignored and noted before driver matching", "[euacx][parser]")
{
    uint8_t desc[sizeof(cx31993_fs_config)]; memcpy(desc, cx31993_fs_config, sizeof(desc));
    for (size_t off = 0; off < sizeof(desc); off += desc[off]) if (desc[off + 1] == 4 && desc[off + 5] == 1) desc[off + 7] = 0;
    euacx_dev_caps_t caps; TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, euacx_parse(desc, sizeof(desc), &caps));
    TEST_ASSERT_TRUE(caps.saw_uac1); TEST_ASSERT_EQUAL(0, caps.num_alts);
}

TEST_CASE("behavior: AS control parsed and read-only Feature Unit not probed", "[euacx][parser]")
{
    init(0);
    uint8_t desc[sizeof(cx31993_fs_config)]; memcpy(desc, cx31993_fs_config, sizeof(desc));
    bool streaming = false;
    for (size_t off = 0; off < sizeof(desc); off += desc[off]) {
        if (desc[off + 1] == 4) streaming = desc[off + 5] == 1 && desc[off + 6] == 2;
        if (streaming && desc[off + 1] == 0x24 && desc[off + 2] == 1) desc[off + 4] = 4;
    }
    TEST_ASSERT_EQUAL(ESP_OK, euacx_parse(desc, sizeof(desc), &dev.caps));
    TEST_ASSERT_EQUAL(4, dev.caps.alts[0].as_controls);
    dev.caps.features[0].volume_write = dev.caps.features[0].mute_write = 0;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port));
    TEST_ASSERT_FALSE(port.info.has_volume); TEST_ASSERT_FALSE(port.info.has_mute);
    TEST_ASSERT_EQUAL(0, fake.count);
}
TEST_CASE("behavior: bitmap indexes high alt numbers and caps remain unchanged on rejection", "[euacx][stream]")
{
    init(0); dev.caps.num_alts = 1; dev.caps.alts[0].alt = 200; dev.caps.alts[0].as_controls = 4;
    fake.bitmap_size = 26; fake.valid[25] = 1;
    euacx_alt_t selected; TEST_ASSERT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(200, selected.alt); euacx_release_interface(&port);
    fake.valid[25] = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, euacx_prepare_stream(&port, &cfg, &selected));
    TEST_ASSERT_EQUAL(1, dev.caps.alts[0].rates.num_rates);
    TEST_ASSERT_EQUAL(-1, dev.claimed); TEST_ASSERT_EQUAL(0, fake.alt);
}
TEST_CASE("behavior: failed or malformed valid-alt read idles and releases claimed interface", "[euacx][stream]")
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        init(EUACX_DRV_ALT_BEFORE_RATE); dev.caps.alts[0].as_controls = 4;
        if (scenario == 0) fake.fail_at = 8;
        else if (scenario == 1) fake.bitmap_size = 0;
        else fake.valid[0] = 0xfe;
        euacx_alt_t selected;
        TEST_ASSERT_NOT_EQUAL(ESP_OK, euacx_prepare_stream(&port, &cfg, &selected));
        TEST_ASSERT_EQUAL(-1, dev.claimed); TEST_ASSERT_EQUAL(0, fake.alt);
        TEST_ASSERT_EQUAL(fake.claims, fake.releases);
    }
}
TEST_CASE("behavior: failed class request still observes control delay", "[euacx][control]")
{
    init(EUACX_DRV_CTL_DELAY); params.ctl_delay_us = 10; fake.fail_at = 1;
    uint8_t value = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, euacx_control(&port, 0xa1, 1, 0x0100, 0x0200, &value, 1));
    TEST_ASSERT_EQUAL(2, fake.count); TEST_ASSERT_EQUAL(0xfc, fake.events[1].type);
    TEST_ASSERT_EQUAL(10, fake.events[1].us);
}

TEST_CASE("behavior: minimum-volume mute survives stream warmup and failed writes do not change target", "[euacx][control]")
{
    init(EUACX_DRV_VOL_MIN_IS_MUTE); TEST_ASSERT_EQUAL(ESP_OK, euacx_probe_controls(&port));
    euacx_stream_state_t stream = {0}; atomic_store(&stream.warming, true); port.stream = &stream;
    int16_t volume = fake.min;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_hw_volume(&port, true, &volume));
    TEST_ASSERT_TRUE(stream.restore_mute); TEST_ASSERT_TRUE(stream.restore_mute_valid);
    stream.restore_mute = stream.restore_mute_valid = false;
    fake.fail_at = fake.count + 1; volume = fake.min;
    TEST_ASSERT_EQUAL(ESP_FAIL, euacx_hw_volume(&port, true, &volume));
    TEST_ASSERT_FALSE(stream.restore_mute); TEST_ASSERT_FALSE(stream.restore_mute_valid);
    port.stream = NULL;
}
