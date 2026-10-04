#include "uac2_internal.h"

size_t uac2_dop_pack(dop_context_t *ctx, const uint8_t *dsd, size_t dsd_len,
                     uint8_t channels, uint8_t *dst, size_t dst_size)
{
    if (!ctx || !channels) return 0;
    size_t in_frame = 2u * channels, out_frame = 3u * channels;
    size_t frames = dsd_len / in_frame;
    if (dst_size / out_frame < frames) frames = dst_size / out_frame;
    for (size_t f = 0; f < frames; f++) {
        for (uint8_t c = 0; c < channels; c++) {
            const uint8_t *s = dsd + f * in_frame + 2u * c;
            uint8_t *o = dst + f * out_frame + 3u * c;
            o[0] = s[1]; o[1] = s[0]; o[2] = ctx->marker;
        }
        ctx->marker = (ctx->marker == 0x05) ? 0xFA : 0x05;
    }
    return frames * out_frame;
}

static esp_err_t dop_probe(const uac2_device_caps_t *d, const uac2_stream_cap_t *c, const uac2_stream_config_t *cfg)
{ (void)d; (void)c; (void)cfg; return ESP_OK; }
const uac2_format_driver_t uac2_dop_driver = { .probe = dop_probe };
