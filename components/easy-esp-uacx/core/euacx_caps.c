/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include "euacx_model.h"

uint32_t euacx_service_ticks(const euacx_alt_t *a) { return a->interval && a->interval <= 16 ? 1u << (a->interval - 1) : 0; }
bool euacx_alt_fits(const euacx_alt_t *a, euacx_speed_t speed, uint32_t hz, uint8_t bits)
{
    uint32_t period = euacx_service_ticks(a), base = speed == EUACX_SPEED_HS ? 8000 : 1000;
    if (!period || bits > a->bits || a->subslot * 8 < a->bits || !hz ||
        (a->feedback_ep && (!a->feedback_interval || a->feedback_interval > 16 ||
                           a->feedback_mps < 3 || a->feedback_mps > 4 ||
                           (speed == EUACX_SPEED_HS && a->feedback_mps != 4)))) return false;
    bool rate = false;
    for (unsigned r = 0; r < a->rates.num_rates; ++r) if (hz == a->rates.rates[r]) rate = true;
    uint64_t frames = ((uint64_t)hz * period + base - 1) / base;
    /* Reserve a whole sample frame for feedback-driven clock variation. */
    if (a->feedback_ep) ++frames;
    uint32_t host_limit = speed == EUACX_SPEED_HS ? 3072 : 1023;
    return rate && frames * 2 * a->subslot <= a->mps && a->mps <= host_limit;
}
const euacx_alt_t *euacx_select(const euacx_dev_caps_t *d, euacx_speed_t speed, const euacx_stream_config_t *req)
{
    return euacx_select_verified(d, speed, req, NULL, false);
}
const euacx_alt_t *euacx_select_verified(const euacx_dev_caps_t *d, euacx_speed_t speed,
                                        const euacx_stream_config_t *req, const euacx_driver_t *driver,
                                        bool verified)
{
    return euacx_select_candidates(d, speed, req, driver, verified, 0);
}
const euacx_alt_t *euacx_select_candidates(const euacx_dev_caps_t *d, euacx_speed_t speed,
                                          const euacx_stream_config_t *req, const euacx_driver_t *driver,
                                          bool verified, uint32_t excluded)
{
    if (!d || !req || req->format != EUACX_FORMAT_PCM || req->channels != 2 ||
        (req->bits != 16 && req->bits != 24 && req->bits != 32)) return NULL;
    const euacx_alt_t *best = NULL;
    const euacx_verified_caps_t *v = verified && driver ? driver->verified : NULL;
    unsigned count = v ? (speed == EUACX_SPEED_HS ? v->num_hs : v->num_fs) : 0;
    const euacx_verified_pcm_t *list = count ? (speed == EUACX_SPEED_HS ? v->hs : v->fs) : NULL;
    for (unsigned i = 0; i < d->num_alts; ++i) {
        if (excluded & (1u << i)) continue;
        const euacx_alt_t *a = &d->alts[i];
        bool allowed = !list;
        for (unsigned x = 0; list && x < count; ++x) {
            if (list[x].bits != req->bits || list[x].subslot != a->subslot) continue;
            for (unsigned r = 0; r < list[x].rates.num_rates; ++r)
                if (list[x].rates.rates[r] == req->sample_rate) allowed = true;
        }
        if (!allowed) continue;
        if (euacx_alt_fits(a, speed, req->sample_rate, req->bits) &&
            (!best || a->bits < best->bits || (a->bits == best->bits &&
             (a->subslot < best->subslot || (a->subslot == best->subslot && a->mps < best->mps))))) best = a;
    }
    return best;
}
void euacx_build_info(const euacx_dev_caps_t *d, const euacx_driver_t *driver, bool verified, euacx_info_t *out)
{
    memset(out->pcm, 0, sizeof(out->pcm)); memset(&out->dsd, 0, sizeof(out->dsd));
    out->driver = driver->name;
    out->driver_flags = driver->flags;
    const euacx_verified_caps_t *v = verified ? driver->verified : NULL;
    const euacx_verified_pcm_t *list = v ? (out->speed == EUACX_SPEED_HS ? v->hs : v->fs) : NULL;
    unsigned count = v ? (out->speed == EUACX_SPEED_HS ? v->num_hs : v->num_fs) : 0;
    out->verified = v && list && count;
    for (unsigned b = 0; b < 3; ++b) {
        uint8_t bits = 16 + b * 8;
        euacx_rate_list_t candidates = {0};
        /* Scan the bounded alt lists; insert each selected rate in ascending order. */
        for (unsigned i = 0; i < d->num_alts; ++i) for (unsigned r = 0; r < d->alts[i].rates.num_rates; ++r) {
            uint32_t hz = d->alts[i].rates.rates[r];
            const euacx_alt_t *a = &d->alts[i];
            bool allow = !out->verified;
            for (unsigned x = 0; out->verified && x < count; ++x) {
                if (list[x].bits != bits || a->subslot != list[x].subslot) continue;
                for (unsigned y = 0; y < list[x].rates.num_rates; ++y) if (list[x].rates.rates[y] == hz) allow = true;
            }
            if (!allow || !euacx_alt_fits(a, out->speed, hz, bits)) continue;
            unsigned pos = 0;
            while (pos < candidates.num_rates && candidates.rates[pos] < hz) ++pos;
            if ((pos < candidates.num_rates && candidates.rates[pos] == hz) || candidates.num_rates == EUACX_MAX_RATES) continue;
            memmove(candidates.rates + pos + 1, candidates.rates + pos, (candidates.num_rates - pos) * sizeof(uint32_t));
            candidates.rates[pos] = hz; ++candidates.num_rates;
        }
        out->pcm[b] = candidates;
    }
}
int16_t euacx_volume_snap(int16_t value, int16_t min, int16_t max, int16_t step)
{
    int32_t v = value;
    if (v < min) v = min;
    if (v > max) v = max;
    if (step > 0) v = min + ((v - min) / step) * step;
    return v;
}
size_t euacx_pcm_convert(const uint8_t *src, size_t samples, uint8_t bits, uint8_t slot, uint8_t *dst)
{
    unsigned in = bits / 8;
    if (!src || !dst || (bits != 16 && bits != 24 && bits != 32) || slot < in || slot > 4) return 0;
    /* Bytes preserve two's complement without signed shifts or overflow. */
    for (size_t i = 0; i < samples; ++i) {
        memset(dst + i * slot, 0, slot - in);
        memcpy(dst + i * slot + slot - in, src + i * in, in);
    }
    return samples * slot;
}
bool euacx_feedback_rate(const uint8_t *p, size_t len, euacx_speed_t speed, uint32_t nominal, uint32_t *q16)
{
    if (!p || !q16 || (len != 3 && len != 4) || (speed == EUACX_SPEED_HS && len != 4)) return false;
    uint32_t raw = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    if (len == 4) raw |= (uint32_t)p[3] << 24;
    uint64_t hz = (uint64_t)raw * (speed == EUACX_SPEED_HS ? 8000 : 1000);
    if (len == 3) hz <<= 2; /* 10.14 -> 16.16 */
    uint64_t expected = (uint64_t)nominal << 16;
    if (hz < expected * 99 / 100 || hz > expected * 101 / 100) return false;
    /* Samples per base frame/microframe, Q16 (avoids rate*65536 overflow). */
    *q16 = len == 3 ? raw << 2 : raw;
    return true;
}

esp_err_t euacx_state_check(euacx_state_t state, euacx_operation_t op,
                            bool pull, bool owner, bool feed, bool aborted)
{
    if (op == EUACX_OP_ABORT) return ESP_OK;
    if (op == EUACX_OP_CLOSE) {
        if (state != EUACX_STATE_STREAMING) return ESP_OK;
        return feed || (!pull && !owner) ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    if (op == EUACX_OP_OPEN) return state == EUACX_STATE_CONNECTED && !feed ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (op == EUACX_OP_WRITE) return state == EUACX_STATE_STREAMING && !pull && owner && !aborted ? ESP_OK : ESP_ERR_INVALID_STATE;
    return state == EUACX_STATE_CONNECTED || state == EUACX_STATE_STREAMING ? ESP_OK : ESP_ERR_INVALID_STATE;
}
