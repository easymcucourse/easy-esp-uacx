/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include "easy_uacx.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "euacx_runtime.h"
#include <stdlib.h>

typedef struct { SemaphoreHandle_t done; esp_err_t result; } callback_deinit_t;
static void deinit_from_callback(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t error, void *user)
{
    (void)port; (void)reason; (void)error;
    callback_deinit_t *r = user;
    r->result = euacx_deinit();
    xSemaphoreGive(r->done);
}
TEST_CASE("runtime: callback can deinit without waiting for itself", "[euacx][lifecycle]")
{
    callback_deinit_t result = {.done = xSemaphoreCreateBinary(), .result = ESP_FAIL};
    TEST_ASSERT_NOT_NULL(result.done);
    euacx_config_t cfg = EUACX_CONFIG_DEFAULT();
    cfg.cb.on_stream_stopped = deinit_from_callback; cfg.user = &result;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_init(&cfg));
    euacx_port_t *port = euacx_get_port(0);
    euacx_notice_t *n = calloc(1, sizeof(*n));
    TEST_ASSERT_NOT_NULL(n);
    n->id = NOTICE_STOPPED; n->port = port;
    /* Inject a terminal event to exercise the real callback task even if no
     * DAC is present. The callback uses only the public lifecycle API. */
    euacx_notice_send(port->ctx, n);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(result.done, pdMS_TO_TICKS(3000)));
    TEST_ASSERT_EQUAL(ESP_OK, result.result);
    vTaskDelay(pdMS_TO_TICKS(200));
    TEST_ASSERT_EQUAL_INT(0, euacx_port_count());
    cfg.cb.on_stream_stopped = NULL; cfg.user = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_init(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, euacx_deinit());
    vTaskDelay(pdMS_TO_TICKS(200));
    vSemaphoreDelete(result.done);
}

/* Real FreeRTOS/USB library lifecycle, with or without a connected DAC.
 * PCM and USB transfer timing are validated separately by the demo. */
TEST_CASE("runtime: init/deinit is repeatable and restores heap", "[euacx][lifecycle]")
{
    euacx_config_t cfg = EUACX_CONFIG_DEFAULT();
    size_t baseline = esp_get_free_heap_size();
    for (unsigned i = 0; i < 3; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, euacx_init(&cfg));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_init(&cfg));
        TEST_ASSERT_EQUAL_INT(1, euacx_port_count());
        TEST_ASSERT_NOT_NULL(euacx_get_port(0));
        TEST_ASSERT_NULL(euacx_get_port(-1)); TEST_ASSERT_NULL(euacx_get_port(1));
        vTaskDelay(pdMS_TO_TICKS(100));
        TEST_ASSERT_EQUAL(ESP_OK, euacx_deinit());
        TEST_ASSERT_EQUAL_INT(0, euacx_port_count());
        TEST_ASSERT_NULL(euacx_get_port(0));
        vTaskDelay(pdMS_TO_TICKS(200));
        TEST_ASSERT_INT_WITHIN(256, baseline, esp_get_free_heap_size());
    }
}
