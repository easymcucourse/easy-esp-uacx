#include "uac2_internal.h"

static esp_err_t pcm_probe(const uac2_device_caps_t *d, const uac2_stream_cap_t *c, const uac2_stream_config_t *cfg)
{ (void)d; return (c->flags & UAC2_CAP_FLAG_PCM) && cfg->sample_rate >= c->rate_min && cfg->sample_rate <= c->rate_max ? ESP_OK : ESP_ERR_NOT_SUPPORTED; }
const uac2_format_driver_t uac2_pcm_driver = { .probe = pcm_probe };
