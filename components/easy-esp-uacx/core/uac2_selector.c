#include <string.h>
#include "uac2_internal.h"

static uint32_t intervals_per_sec(const uac2_host_hw_caps_t *h) { return h->max_speed ? 8000u : 1000u; }

/* Does this cap carry `rate` with `bits`/`channels` within host + endpoint payload limits? */
static bool fits(const uac2_stream_cap_t *c, const uac2_host_hw_caps_t *h,
                 uint32_t rate, uint8_t bits, uint8_t ch)
{
    if (c->channels < ch || c->bit_resolution < bits || c->subslot_size * 8u < bits) return false;
    if (rate < c->rate_min || rate > c->rate_max) return false;
    uint32_t need = uac2_packet_bytes(rate, c->channels, c->subslot_size, intervals_per_sec(h));
    uint32_t lim = c->max_packet_size < h->max_iso_payload ? c->max_packet_size : h->max_iso_payload;
    return need <= lim;
}

static const uac2_stream_cap_t *find(const uac2_device_caps_t *d, const uac2_host_hw_caps_t *h,
                                     uint32_t rate, uint8_t bits, uint8_t ch, uint8_t need_flags)
{
    for (int i = 0; i < d->num_playback_caps; i++) {
        const uac2_stream_cap_t *c = &d->playback_caps[i];
        if ((c->flags & need_flags) == need_flags && fits(c, h, rate, bits, ch)) return c;
    }
    return NULL;
}

esp_err_t uac2_find_best_mode(const uac2_device_caps_t *dac, const uac2_host_hw_caps_t *host,
                              const uac2_stream_config_t *req,
                              uac2_stream_config_t *chosen, const uac2_stream_cap_t **cap)
{
    if (!dac || !host || !req || !chosen) return ESP_ERR_INVALID_ARG;
    const uac2_stream_cap_t *c = NULL;
    *chosen = *req;
    if (req->format == UAC2_FORMAT_PCM) {
        c = find(dac, host, req->sample_rate, req->bits, req->channels, UAC2_CAP_FLAG_PCM);
    } else {
        /* DSD: prefer native, then DoP */
        if (dac->feature_flags & UAC2_QUIRK_NATIVE_DSD) {
            c = find(dac, host, uac2_dsd_native_rate(req->dsd_rate), 32, req->channels, UAC2_CAP_FLAG_RAW_DATA);
            if (c) { chosen->format = UAC2_FORMAT_DSD_NATIVE; chosen->sample_rate = uac2_dsd_native_rate(req->dsd_rate); chosen->bits = 32; }
        }
        if (!c) {
            c = find(dac, host, uac2_dsd_dop_rate(req->dsd_rate), 24, req->channels, UAC2_CAP_FLAG_PCM);
            if (c) { chosen->format = UAC2_FORMAT_DSD_DOP; chosen->sample_rate = uac2_dsd_dop_rate(req->dsd_rate); chosen->bits = 24; }
        }
    }
    if (!c) return ESP_ERR_NOT_SUPPORTED;
    if (cap) *cap = c;
    return ESP_OK;
}

esp_err_t uac2_get_speed_caps(const uac2_device_caps_t *dac, const uac2_host_hw_caps_t *host,
                              uac2_speed_caps_t *out)
{
    if (!dac || !host || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    for (int i = 0; i < dac->num_playback_caps; i++) {
        const uac2_stream_cap_t *c = &dac->playback_caps[i];
        if (c->flags & UAC2_CAP_FLAG_PCM) {
            if (fits(c, host, c->rate_min, c->bit_resolution, c->channels)) {
                out->supports_pcm = true;
                if (out->min_pcm_rate == 0 || c->rate_min < out->min_pcm_rate) {
                    out->min_pcm_rate = c->rate_min;
                }
                if (c->rate_max > out->max_pcm_rate) {
                    out->max_pcm_rate = c->rate_max;
                }
                if (c->channels > out->max_channels) {
                    out->max_channels = c->channels;
                }
                if (c->bit_resolution == 16) out->bit_depth_mask |= UAC2_BIT_DEPTH_16;
                else if (c->bit_resolution == 24) out->bit_depth_mask |= UAC2_BIT_DEPTH_24;
                else if (c->bit_resolution == 32) out->bit_depth_mask |= UAC2_BIT_DEPTH_32;
            }
        }
    }

    static const uac2_dsd_rate_t dsd_rates[] = { UAC2_DSD64, UAC2_DSD128, UAC2_DSD256 };
    for (size_t i = 0; i < sizeof(dsd_rates) / sizeof(dsd_rates[0]); i++) {
        uac2_dsd_rate_t r = dsd_rates[i];
        if (dac->feature_flags & UAC2_QUIRK_NATIVE_DSD) {
            if (find(dac, host, uac2_dsd_native_rate(r), 32, 2, UAC2_CAP_FLAG_RAW_DATA)) {
                out->supports_dsd = true;
                out->supports_dsd_native = true;
                if (r > out->max_dsd_rate) out->max_dsd_rate = r;
            }
        }
        if (find(dac, host, uac2_dsd_dop_rate(r), 24, 2, UAC2_CAP_FLAG_PCM)) {
            out->supports_dsd = true;
            out->supports_dsd_dop = true;
            if (r > out->max_dsd_rate) out->max_dsd_rate = r;
        }
    }

    return ESP_OK;
}

esp_err_t uac2_get_dac_caps(const uac2_device_caps_t *dac, uac2_dac_caps_t *out)
{
    static const uac2_host_hw_caps_t FS = { .max_speed = 0, .max_iso_payload = 1023, .high_bandwidth_iso = false };
    static const uac2_host_hw_caps_t HS = { .max_speed = 1, .max_iso_payload = 3072, .high_bandwidth_iso = true };
    if (!dac || !out) return ESP_ERR_INVALID_ARG;
    esp_err_t e = uac2_get_speed_caps(dac, &FS, &out->fs);
    if (e != ESP_OK) return e;
    return uac2_get_speed_caps(dac, &HS, &out->hs);
}
