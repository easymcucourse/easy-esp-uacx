/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include "euacx_model.h"

static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }
static void save_alt(euacx_dev_caps_t *d, const euacx_alt_t *a, bool pcm, uint8_t channels)
{
    if (pcm && channels == 2 && a->ep && a->interval && a->interval <= 16 &&
        a->subslot >= 2 && a->subslot <= 4 && (a->bits == 16 || a->bits == 24 || a->bits == 32) &&
        a->bits <= a->subslot * 8 && d->num_alts < EUACX_MAX_ALTS) d->alts[d->num_alts++] = *a;
}
esp_err_t euacx_parse(const uint8_t *data, size_t size, euacx_dev_caps_t *d)
{
    if (!data || !d || size < 9 || data[1] != 2 || le16(data + 2) > size) return ESP_ERR_INVALID_ARG;
    memset(d, 0, sizeof(*d));
    size = le16(data + 2);
    uint8_t cls = 0, sub = 0, proto = 0, channels = 0;
    bool pcm = false;
    euacx_alt_t cur = {0};
    for (size_t off = 0; off < size;) {
        const uint8_t *p = data + off;
        if (size - off < 2 || p[0] < 2 || p[0] > size - off) return ESP_ERR_INVALID_SIZE;
        uint8_t len = p[0], type = p[1];
        if (type == 4 && len >= 9) {
            save_alt(d, &cur, pcm, channels);
            memset(&cur, 0, sizeof(cur)); pcm = false; channels = 0;
            cls = p[5]; sub = p[6]; proto = p[7];
            if (cls == 1 && (sub == 1 || sub == 2) && proto == 0) d->saw_uac1 = true;
            if (cls == 1 && sub == 1 && proto == 0x20) d->ac_interface = p[2];
            if (cls == 1 && sub == 2 && proto == 0x20 && p[3]) {
                cur.interface = p[2]; cur.alt = p[3];
            }
        } else if (type == 0x24 && cls == 1 && proto == 0x20 && sub == 1 && len >= 4) {
            uint8_t id = p[3];
            if (p[2] == 2 && len >= 17) d->clock[id] = p[7]; /* input terminal */
            if (p[2] == 3 && len >= 12) { d->source[id] = p[7]; d->clock[id] = p[8]; }
            if (p[2] == 0x0a && len >= 8) {
                if (!d->first_clock) d->first_clock = id;
                d->clock[id] = id; d->clock_controls[id] = p[5];
            }
            if (p[2] == 6 && len >= 10 && (len - 6) % 4 == 0) {
                d->source[id] = p[4];
                if (d->num_features < EUACX_MAX_FEATURES) {
                    euacx_feature_t *f = &d->features[d->num_features++];
                    f->id = id; f->source = p[4];
                    for (unsigned ch = 0; ch < (unsigned)(len - 6) / 4 && ch < 32; ++ch) {
                        uint32_t controls = le32(p + 5 + ch * 4);
                        if (controls & 1) f->mute_read |= 1u << ch;
                        if ((controls & 3) == 3) f->mute_write |= 1u << ch;
                        if (controls & 4) f->volume_read |= 1u << ch;
                        if ((controls & 12) == 12) f->volume_write |= 1u << ch;
                    }
                }
            }
        } else if (type == 0x24 && cls == 1 && proto == 0x20 && sub == 2 && cur.alt) {
            if (len >= 16 && p[2] == 1) { pcm = p[5] == 1 && (le32(p + 6) & 1); channels = p[10]; cur.terminal = p[3]; cur.as_controls = p[4]; }
            if (len >= 6 && p[2] == 2 && p[3] == 1) { cur.subslot = p[4]; cur.bits = p[5]; }
        } else if (type == 5 && len >= 7 && cur.alt && (p[3] & 3) == 1) {
            uint16_t w = le16(p + 4);
            uint16_t mps = (w & 0x7ff) * (1 + ((w >> 11) & 3));
            uint8_t usage = (p[3] >> 4) & 3;
            if (!(p[2] & 0x80) && usage == 0) { cur.ep = p[2]; cur.mps = mps; cur.interval = p[6]; cur.sync = (p[3] >> 2) & 3; }
            if ((p[2] & 0x80) && usage == 1) { cur.feedback_ep = p[2]; cur.feedback_mps = mps; cur.feedback_interval = p[6]; }
        }
        off += len;
    }
    save_alt(d, &cur, pcm, channels);
    for (unsigned i = 0; i < d->num_alts; ++i) {
        euacx_alt_t *a = &d->alts[i];
        uint8_t id = d->clock[a->terminal];
        if (!id || d->clock[id] != id) { id = d->first_clock; d->clock_fallback = true; }
        a->clock = id;
    }
    return d->num_alts && d->first_clock ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

const euacx_feature_t *euacx_feature(const euacx_dev_caps_t *d, uint8_t terminal)
{
    /* Follow output terminals back to the streaming input terminal. */
    for (unsigned i = 0; i < d->num_features; ++i) {
        uint8_t id = d->features[i].id;
        for (unsigned hop = 0; id && hop < 256; ++hop) {
            if (id == terminal) return &d->features[i];
            id = d->source[id];
        }
    }
    uint8_t id = terminal;
    for (unsigned hop = 0; id && hop < 256; ++hop) {
        for (unsigned i = 0; i < d->num_features; ++i) if (d->features[i].id == id) return &d->features[i];
        id = d->source[id];
    }
    return NULL;
}

void euacx_standard_rates(euacx_rate_list_t *out)
{
    static const uint32_t rates[EUACX_MAX_RATES] = {8000,11025,16000,22050,32000,44100,48000,64000,88200,96000,176400,192000,352800,384000,705600,768000};
    out->num_rates = EUACX_MAX_RATES; memcpy(out->rates, rates, sizeof(rates));
}
static void add_rate(euacx_rate_list_t *out, uint32_t hz)
{
    unsigned i = 0;
    while (i < out->num_rates && out->rates[i] < hz) ++i;
    if (!hz || (i < out->num_rates && out->rates[i] == hz) || out->num_rates == EUACX_MAX_RATES) return;
    memmove(out->rates + i + 1, out->rates + i, (out->num_rates - i) * sizeof(uint32_t));
    out->rates[i] = hz; ++out->num_rates;
}
esp_err_t euacx_parse_rates(const uint8_t *data, size_t size, euacx_rate_list_t *out)
{
    if (!data || !out || size < 2) return ESP_ERR_INVALID_ARG;
    unsigned n = le16(data);
    if (!n || n > (size - 2) / 12) return ESP_ERR_INVALID_SIZE;
    memset(out, 0, sizeof(*out));
    euacx_rate_list_t standard; euacx_standard_rates(&standard);
    for (unsigned i = 0; i < n; ++i) {
        const uint8_t *p = data + 2 + i * 12;
        uint32_t min = le32(p), max = le32(p + 4), res = le32(p + 8);
        if (!min || min > max) return ESP_ERR_INVALID_RESPONSE;
        if (min == max) add_rate(out, min);
        else for (unsigned r = 0; r < standard.num_rates; ++r) {
            uint32_t hz = standard.rates[r];
            if (hz >= min && hz <= max && (!res || (hz - min) % res == 0)) add_rate(out, hz);
        }
    }
    return out->num_rates ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
