#pragma once
#include "uac2_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uac2_host_init(void);
esp_err_t uac2_host_deinit(void);

/** Open first attached UAC2 DAC (blocks up to timeout_ms). */
esp_err_t uac2_open(uac2_device_handle_t *out, uint32_t timeout_ms);
esp_err_t uac2_close(uac2_device_handle_t dev);

esp_err_t uac2_get_capabilities(uac2_device_handle_t dev, const uac2_device_caps_t **caps);
uac2_state_t uac2_get_state(uac2_device_handle_t dev);

esp_err_t uac2_stream_open(uac2_device_handle_t dev, const uac2_stream_config_t *cfg);
int       uac2_write(uac2_device_handle_t dev, const void *data, size_t len, uint32_t timeout_ms);
esp_err_t uac2_stream_close(uac2_device_handle_t dev);

/** Selector: pick best mode given DAC + host caps; ESP_ERR_NOT_SUPPORTED if bandwidth/format fails. */
esp_err_t uac2_find_best_mode(const uac2_device_caps_t *dac, const uac2_host_hw_caps_t *host,
                              const uac2_stream_config_t *requested,
                              uac2_stream_config_t *chosen, const uac2_stream_cap_t **cap);

#ifdef __cplusplus
}
#endif
