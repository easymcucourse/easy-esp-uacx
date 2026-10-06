/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include <limits.h>
#include "esp_log.h"
#include "euacx_runtime.h"

static uint32_t flags(const euacx_port_t *p) { return p->dev->driver->flags; }
esp_err_t euacx_control(euacx_port_t *p, uint8_t type, uint8_t req,
                        uint16_t val, uint16_t index, void *data, uint16_t size)
{
    esp_err_t e = euacx_control_transfer(p, type, req, val, index, data, size);
    if ((type & 0x60) == 0x20 && (flags(p) & EUACX_DRV_CTL_DELAY))
        euacx_control_delay(p->dev->driver->params->ctl_delay_us);
    return e;
}
esp_err_t euacx_set_interface(euacx_port_t *p, uint8_t interface, uint8_t alt)
{
    esp_err_t e = euacx_control(p, 1, 0x0b, alt, interface, NULL, 0);
    if (e == ESP_OK && alt && (flags(p) & EUACX_DRV_IFACE_DELAY))
        euacx_control_delay((uint32_t)p->dev->driver->params->iface_delay_ms * 1000);
    uint8_t actual = 0xff;
    if (e == ESP_OK) e = euacx_control(p, 0x81, 0x0a, 0, interface, &actual, 1);
    return e == ESP_OK && actual != alt ? ESP_ERR_INVALID_RESPONSE : e;
}
esp_err_t euacx_clock_rate(euacx_port_t *p, const euacx_alt_t *a, uint32_t hz)
{
    uint8_t data[4] = {hz, hz >> 8, hz >> 16, hz >> 24};
    uint16_t entity = (a->clock << 8) | p->dev->caps.ac_interface;
    esp_err_t e = euacx_control(p, 0x21, 1, 0x0100, entity, data, 4);
    if (e == ESP_OK && !(flags(p) & EUACX_DRV_RATE_NO_READBACK))
        e = euacx_control(p, 0xa1, 1, 0x0100, entity, data, 4);
    uint32_t actual = data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    if (e == ESP_OK && actual != hz) e = ESP_ERR_NOT_SUPPORTED;
    if (e == ESP_OK && (p->dev->caps.clock_controls[a->clock] & 4)) {
        uint8_t valid = 0;
        e = euacx_control(p, 0xa1, 1, 0x0200, entity, &valid, 1);
        if (e == ESP_OK && !valid) e = ESP_ERR_INVALID_STATE;
    }
    return e;
}
void euacx_probe_rates(euacx_port_t *p)
{
    euacx_dev_caps_t *d = &p->dev->caps;
    for (unsigned a = 0; a < d->num_alts; ++a) {
        euacx_alt_t *alt = &d->alts[a];
        bool cached = false;
        for (unsigned b = 0; b < a; ++b) if (d->alts[b].clock == alt->clock) { alt->rates = d->alts[b].rates; cached = true; break; }
        if (cached) continue;
        uint16_t entity = (alt->clock << 8) | d->ac_interface;
        uint8_t n[2] = {0};
        esp_err_t e = euacx_control(p, 0xa1, 2, 0x0100, entity, n, 2);
        unsigned count = n[0] | (unsigned)n[1] << 8;
        if (e == ESP_OK && count && count <= 32) {
            uint8_t data[2 + 32 * 12];
            e = euacx_control(p, 0xa1, 2, 0x0100, entity, data, 2 + count * 12);
            if (e == ESP_OK) e = euacx_parse_rates(data, 2 + count * 12, &alt->rates);
        } else e = ESP_ERR_NOT_SUPPORTED;
        if (e != ESP_OK) {
            euacx_standard_rates(&alt->rates);
            ESP_LOGW("euacx", "clock %u GET RANGE unavailable; SET/GET will validate open", alt->clock);
        }
    }
}
static unsigned first_channel(uint32_t mask)
{
    for (unsigned i = 0; i < 3; ++i) if (mask & (1u << i)) return i;
    return 32;
}
/* Raw Feature Unit requests also serve the reversible enumeration probe. */
static esp_err_t feature_value(euacx_port_t *p, unsigned ch, bool volume, bool set, int16_t *value)
{
    uint8_t data[2] = {(uint8_t)*value, (uint16_t)*value >> 8};
    esp_err_t e = euacx_control(p, set ? 0x21 : 0xa1, 1, (volume ? 0x0200 : 0x0100) | ch,
                               (p->dev->feature->id << 8) | p->dev->caps.ac_interface,
                               data, volume ? 2 : 1);
    if (e == ESP_OK && !set) *value = volume ? (int16_t)(data[0] | (unsigned)data[1] << 8) : !!data[0];
    return e;
}
static bool probe_readback(euacx_port_t *p, bool volume, unsigned ch)
{
    int16_t original = 0;
    if (feature_value(p, ch, volume, false, &original) != ESP_OK) return false;
    int16_t target = !original;
    if (volume) {
        int32_t next = (int32_t)original + p->info.volume_res;
        if (next > p->info.volume_max) next = (int32_t)original - p->info.volume_res;
        if (original == p->dev->volume_raw_min && (flags(p) & EUACX_DRV_VOL_MIN_IS_MUTE)) next = p->info.volume_min;
        else if (original < p->info.volume_min) return false;
        if (original > p->info.volume_max || next < p->info.volume_min) return false;
        target = next;
    }
    esp_err_t write = feature_value(p, ch, volume, true, &target);
    int16_t actual = original;
    esp_err_t read = write == ESP_OK ? feature_value(p, ch, volume, false, &actual) : write;
    /* Restore even when the test write or read failed: an errored SET may have applied. */
    esp_err_t restore = feature_value(p, ch, volume, true, &original);
    return write == ESP_OK && read == ESP_OK && restore == ESP_OK && actual == target;
}
esp_err_t euacx_probe_controls(euacx_port_t *p)
{
    euacx_dev_t *d = p->dev;
    d->volume_cached = d->mute_cached = false;
    d->feature = euacx_feature(&d->caps, d->caps.alts[0].terminal);
    euacx_info_t *info = &p->info;
    info->has_volume = info->has_mute = false;
    if (!d->feature) return ESP_OK;
    const euacx_feature_t *f = d->feature;
    info->has_mute = (f->mute_read & f->mute_write & 7) != 0;
    info->has_volume = (f->volume_read & f->volume_write & 7) != 0 && !(flags(p) & EUACX_DRV_NO_HW_VOLUME);
    if (info->has_volume) {
        if (flags(p) & EUACX_DRV_VOL_RANGE) {
            const euacx_driver_params_t *params = d->driver->params;
            info->volume_min = params->vol_min; info->volume_max = params->vol_max; info->volume_res = params->vol_res;
        } else {
            uint8_t range[8] = {0};
            unsigned ch = first_channel(f->volume_read & f->volume_write);
            esp_err_t e = euacx_control(p, 0xa1, 2, 0x0200 | ch,
                                       (f->id << 8) | d->caps.ac_interface, range, sizeof(range));
            unsigned count = range[0] | (unsigned)range[1] << 8;
            if (e != ESP_OK || !count) info->has_volume = false;
            else {
                if (count > 1) ESP_LOGW("euacx", "multiple volume ranges: using first subrange");
                info->volume_min = (int16_t)(range[2] | (unsigned)range[3] << 8);
                info->volume_max = (int16_t)(range[4] | (unsigned)range[5] << 8);
                info->volume_res = (int16_t)(range[6] | (unsigned)range[7] << 8);
            }
        }
        if (!info->volume_res) info->volume_res = 1;
        d->volume_raw_min = info->volume_min;
        if (flags(p) & EUACX_DRV_VOL_MIN_IS_MUTE) {
            int32_t min = (int32_t)info->volume_min + info->volume_res;
            if (!info->has_mute || min > INT16_MAX) info->has_volume = false;
            else info->volume_min = min;
        }
        if (info->volume_min >= info->volume_max || info->volume_res < 0 ||
            (int32_t)info->volume_max - info->volume_min < info->volume_res) info->has_volume = false;
    }
    if (!(flags(p) & EUACX_DRV_VOL_NO_READBACK)) {
        if (info->has_volume && !probe_readback(p, true, first_channel(f->volume_read & f->volume_write))) {
            info->has_volume = false;
            ESP_LOGW("euacx", "VOLUME READBACK FAILED %04x:%04x", info->vid, info->pid);
        }
        if (info->has_mute && !probe_readback(p, false, first_channel(f->mute_read & f->mute_write))) {
            info->has_mute = false;
            ESP_LOGW("euacx", "MUTE READBACK FAILED %04x:%04x", info->vid, info->pid);
        }
    } else if (info->has_volume) {
        /* No trustworthy initial CUR: establish a quiet value by a successful SET. */
        int16_t quiet = info->volume_min;
        if (euacx_hw_volume(p, true, &quiet) != ESP_OK) info->has_volume = false;
    }
    bool mute = false;
    if (info->has_mute && euacx_hw_mute(p, true, &mute) != ESP_OK) info->has_mute = false;
    return ESP_OK;
}
esp_err_t euacx_hw_volume(euacx_port_t *p, bool set, int16_t *value)
{
    if (!p->info.has_volume || !p->dev->feature) return ESP_ERR_NOT_SUPPORTED;
    euacx_dev_t *d = p->dev;
    bool cached = (flags(p) & EUACX_DRV_VOL_NO_READBACK) != 0;
    if (!set && cached) {
        if (!d->volume_cached) return ESP_ERR_INVALID_STATE;
        *value = d->volume_cache; return ESP_OK;
    }
    if (set && (flags(p) & EUACX_DRV_VOL_MIN_IS_MUTE) && *value <= d->volume_raw_min) {
        bool on = true;
        esp_err_t e = euacx_hw_mute(p, true, &on);
        /* A mute request does not change the last successful volume SET. */
        if (e == ESP_OK) {
            *value = d->volume_raw_min;
            if (p->stream && atomic_load(&p->stream->warming)) {
                p->stream->restore_mute = true; p->stream->restore_mute_valid = true;
            }
        }
        return e;
    }
    const euacx_feature_t *f = d->feature;
    uint32_t mask = f->volume_read & f->volume_write & 7;
    if (!set) mask = 1u << first_channel(mask);
    int16_t target = euacx_volume_snap(*value, p->info.volume_min, p->info.volume_max, p->info.volume_res);
    if (set) d->volume_cached = false;
    for (unsigned ch = 0; ch < 3; ++ch) if (mask & (1u << ch)) {
        int16_t actual = target;
        esp_err_t e = set ? feature_value(p, ch, true, true, &actual) : ESP_OK;
        if (e == ESP_OK && (!set || !cached)) e = feature_value(p, ch, true, false, &actual);
        if (e != ESP_OK) return e;
        if (set && actual != target) return ESP_ERR_INVALID_RESPONSE;
        *value = actual;
    }
    d->volume_cache = *value; d->volume_cached = true;
    return ESP_OK;
}
esp_err_t euacx_hw_mute(euacx_port_t *p, bool set, bool *value)
{
    if (!p->info.has_mute || !p->dev->feature) return ESP_ERR_NOT_SUPPORTED;
    euacx_dev_t *d = p->dev;
    bool cached = (flags(p) & EUACX_DRV_VOL_NO_READBACK) != 0;
    if (!set && cached) {
        if (!d->mute_cached) return ESP_ERR_INVALID_STATE;
        *value = d->mute_cache; return ESP_OK;
    }
    const euacx_feature_t *f = d->feature;
    uint32_t mask = f->mute_read & f->mute_write & 7;
    if (!set) mask = 1u << first_channel(mask);
    bool target = *value;
    if (set) d->mute_cached = false;
    for (unsigned ch = 0; ch < 3; ++ch) if (mask & (1u << ch)) {
        int16_t actual = target;
        esp_err_t e = set ? feature_value(p, ch, false, true, &actual) : ESP_OK;
        if (e == ESP_OK && (!set || !cached)) e = feature_value(p, ch, false, false, &actual);
        if (e != ESP_OK) return e;
        if (set && (bool)actual != target) return ESP_ERR_INVALID_RESPONSE;
        *value = actual != 0;
    }
    d->mute_cache = *value; d->mute_cached = true;
    return ESP_OK;
}

static esp_err_t valid_alt(euacx_port_t *p, const euacx_alt_t *a, bool *valid)
{
    *valid = true;
    if (!(a->as_controls & 4)) return ESP_OK;
    uint8_t count = 0;
    esp_err_t e = euacx_control(p, 0xa1, 1, 0x0200, a->interface, &count, 1);
    if (e != ESP_OK) return e;
    if (!count || count > 32) return ESP_ERR_INVALID_RESPONSE;
    uint8_t data[33] = {0};
    e = euacx_control(p, 0xa1, 1, 0x0200, a->interface, data, count + 1);
    if (e != ESP_OK) return e;
    if (data[0] != count || !(data[1] & 1)) return ESP_ERR_INVALID_RESPONSE;
    *valid = a->alt / 8 < count && (data[1 + a->alt / 8] & (1u << (a->alt % 8)));
    return ESP_OK;
}
esp_err_t euacx_prepare_stream(euacx_port_t *p, const euacx_stream_config_t *cfg, euacx_alt_t *selected)
{
    /* A bit per parsed candidate avoids copying several KB onto the manager stack. */
    const euacx_dev_caps_t *candidates = &p->dev->caps;
    uint32_t excluded = 0;
    bool early = (flags(p) & EUACX_DRV_ALT_BEFORE_RATE) != 0;
    for (unsigned attempt = 0; attempt < candidates->num_alts; ++attempt) {
        const euacx_alt_t *a = euacx_select_candidates(candidates, p->info.speed, cfg, p->dev->driver, p->info.verified, excluded);
        if (!a) return ESP_ERR_NOT_SUPPORTED;
        esp_err_t e = euacx_claim_interface(p, a->interface, 0);
        if (e == ESP_OK) e = euacx_set_interface(p, a->interface, 0);
        if (e == ESP_OK && early) {
            euacx_release_interface(p);
            e = euacx_claim_interface(p, a->interface, a->alt);
            if (e == ESP_OK) e = euacx_set_interface(p, a->interface, a->alt);
        }
        if (e == ESP_OK) e = euacx_clock_rate(p, a, cfg->sample_rate);
        bool valid = false;
        if (e == ESP_OK) e = valid_alt(p, a, &valid);
        if (e == ESP_OK && valid && !early) {
            euacx_release_interface(p);
            e = euacx_claim_interface(p, a->interface, a->alt);
            if (e == ESP_OK) e = euacx_set_interface(p, a->interface, a->alt);
        }
        if (e == ESP_OK && valid) { *selected = *a; return ESP_OK; }
        /* An early-selected alt must be idled before trying another candidate. */
        esp_err_t idle = ESP_OK;
        if (p->dev->claimed >= 0 && !atomic_load(&p->gone)) idle = euacx_set_interface(p, a->interface, 0);
        euacx_release_interface(p);
        if (e != ESP_OK) return e;
        if (idle != ESP_OK) return idle;
        excluded |= 1u << (a - candidates->alts);
    }
    return ESP_ERR_NOT_SUPPORTED;
}
