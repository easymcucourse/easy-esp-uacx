/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <stdatomic.h>
#include "easy_uacx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

typedef struct {
    euacx_port_t *port;
    euacx_info_t info;
    SemaphoreHandle_t done, entered;
    bool pull;
    esp_err_t write, close, abort, volume;
    TickType_t elapsed;
} validation_t;
static void foreign_task(void *arg)
{
    validation_t *v = arg;
    uint8_t byte = 0;
    vTaskDelay(pdMS_TO_TICKS(20));
    TickType_t start = xTaskGetTickCount();
    v->volume = ESP_OK;
    if (v->info.has_volume) {
        int16_t volume = 0, got = 0;
        v->volume = euacx_get_volume(v->port, &volume);
        if (v->volume == ESP_OK) v->volume = euacx_set_volume(v->port, volume);
        if (v->volume == ESP_OK) v->volume = euacx_get_volume(v->port, &got);
        if (v->volume == ESP_OK && got != volume) v->volume = ESP_FAIL;
    }
    v->elapsed = xTaskGetTickCount() - start;
    if (!v->pull) {
        v->write = euacx_write(v->port, &byte, 1, NULL, 0);
        v->close = euacx_stream_close(v->port);
        v->abort = euacx_stream_abort(v->port);
    }
    SemaphoreHandle_t done = v->done;
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}
static int blocked_data(euacx_port_t *port, void *buf, size_t len, void *arg)
{
    (void)buf; (void)len;
    validation_t *v = arg;
    v->close = euacx_stream_close(port); /* Must reject closing oneself. */
    xSemaphoreGive(v->entered);
    vTaskDelay(pdMS_TO_TICKS(1000));
    return EUACX_DATA_END;
}
unsigned easy_uacx_validate(euacx_port_t *port, const euacx_info_t *info, SemaphoreHandle_t stopped)
{
    if (!info->pcm[1].num_rates) return 0;
    unsigned failed = 0;
    euacx_stream_config_t cfg = {.format = EUACX_FORMAT_DSD, .dsd_rate = EUACX_DSD64, .channels = 2};
    if (euacx_stream_open(port, &cfg, NULL) != ESP_ERR_NOT_SUPPORTED) ++failed;
    cfg.format = EUACX_FORMAT_DOP; cfg.bits = 24;
    if (euacx_stream_open(port, &cfg, NULL) != ESP_ERR_NOT_SUPPORTED) ++failed;
    cfg.format = EUACX_FORMAT_PCM; cfg.sample_rate = info->pcm[1].rates[0];
    for (unsigned i = 0; i < info->pcm[1].num_rates; ++i) if (info->pcm[1].rates[i] == 48000) cfg.sample_rate = 48000;
    validation_t v = {.port = port, .info = *info, .done = xSemaphoreCreateBinary(), .entered = xSemaphoreCreateBinary()};
    if (!v.done || !v.entered) { if (v.done) vSemaphoreDelete(v.done); if (v.entered) vSemaphoreDelete(v.entered); return failed + 1; }
    while (xSemaphoreTake(stopped, 0) == pdTRUE) {}
    if (euacx_stream_open(port, &cfg, NULL) == ESP_OK) {
        uint8_t *data = calloc(1, 131072);
        if (data && xTaskCreate(foreign_task, "euacx_ui_test", 4096, &v, 5, NULL) == pdPASS) {
            size_t written = 0;
            esp_err_t e = euacx_write(port, data, 131072, &written, 2000);
            xSemaphoreTake(v.done, portMAX_DELAY);
            bool ok = e == ESP_ERR_INVALID_STATE && written > 0 && written < 131072 &&
                v.write == ESP_ERR_INVALID_STATE && v.close == ESP_ERR_INVALID_STATE &&
                v.abort == ESP_OK && v.volume == ESP_OK && euacx_get_state(port) == EUACX_STATE_STREAMING;
            if (!ok) ++failed;
            ESP_LOGI("euacx_test", "RUNTIME push owner/abort/volume status=%s bytes=%u volume_ms=%u", ok ? "PASS" : "FAIL", (unsigned)written, (unsigned)(v.elapsed * portTICK_PERIOD_MS));
        } else ++failed;
        free(data);
        if (euacx_stream_close(port) != ESP_OK || xSemaphoreTake(stopped, pdMS_TO_TICKS(3000)) != pdTRUE) ++failed;
    } else ++failed;
    while (xSemaphoreTake(stopped, 0) == pdTRUE) {}
    v.pull = true; cfg.on_data = blocked_data; cfg.data_user = &v;
    if (euacx_stream_open(port, &cfg, NULL) == ESP_OK) {
        if (xSemaphoreTake(v.entered, pdMS_TO_TICKS(3000)) == pdTRUE &&
            xTaskCreate(foreign_task, "euacx_ui_test", 4096, &v, 5, NULL) == pdPASS) {
            uint8_t byte = 0;
            esp_err_t write = euacx_write(port, &byte, 1, NULL, 0);
            esp_err_t close = euacx_stream_close(port);
            xSemaphoreTake(v.done, portMAX_DELAY);
            bool ok = write == ESP_ERR_INVALID_STATE && v.close == ESP_ERR_INVALID_STATE && close == ESP_OK &&
                v.volume == ESP_OK && v.elapsed < pdMS_TO_TICKS(500);
            if (!ok) ++failed;
            ESP_LOGI("euacx_test", "RUNTIME pull blocked-feed/close/volume status=%s volume_ms=%u", ok ? "PASS" : "FAIL", (unsigned)(v.elapsed * portTICK_PERIOD_MS));
        } else { ++failed; euacx_stream_close(port); }
        if (xSemaphoreTake(stopped, pdMS_TO_TICKS(3000)) != pdTRUE) ++failed;
    } else ++failed;
    vSemaphoreDelete(v.done); vSemaphoreDelete(v.entered);
    ESP_LOGI("euacx_test", "RUNTIME SUMMARY failed=%u", failed);
    return failed;
}
