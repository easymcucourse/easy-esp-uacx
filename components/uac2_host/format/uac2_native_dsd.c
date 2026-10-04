#include "uac2_internal.h"

static esp_err_t nd_probe(const uac2_device_caps_t *d, const uac2_stream_cap_t *c, const uac2_stream_config_t *cfg)
{
    (void)cfg;
    return ((d->feature_flags & UAC2_QUIRK_NATIVE_DSD) && (c->flags & UAC2_CAP_FLAG_RAW_DATA)) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
const uac2_format_driver_t uac2_native_dsd_driver = { .probe = nd_probe };
