#include "uac2_internal.h"
#include "esp_log.h"

/* Conexant CX31993 (USB Audio 2.0 DAC/headset chip). VID 0x0572, PID 0x1B08/0x1B09. */
static const char *TAG = "uac2_cx31993";

static esp_err_t cx31993_on_ready(uac2_device_t *dev)
{
    (void)dev;
    ESP_LOGI(TAG, "CX31993 detected");
    return ESP_OK;
}

static esp_err_t cx31993_on_stream_start(uac2_device_t *dev, const uac2_stream_config_t *cfg)
{
    (void)dev; (void)cfg;
    /* TODO: CX31993-specific init (vendor requests) goes here */
    return ESP_OK;
}

static esp_err_t cx31993_on_stream_stop(uac2_device_t *dev)
{
    (void)dev;
    return ESP_OK;
}

const uac2_quirk_ops_t uac2_quirk_cx31993_ops = {
    .on_ready        = cx31993_on_ready,
    .on_stream_start = cx31993_on_stream_start,
    .on_stream_stop  = cx31993_on_stream_stop,
};
