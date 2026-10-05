/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include "euacx_runtime.h"

void euacx_request_reply(euacx_request_t *r, esp_err_t result)
{
    r->result = result;
    xSemaphoreGive(r->done);
}
esp_err_t euacx_request_call(euacx_context_t *c, euacx_request_t *r)
{
    if (xTaskGetCurrentTaskHandle() == c->mgr || xTaskGetCurrentTaskHandle() == c->pump) return ESP_ERR_INVALID_STATE;
    StaticSemaphore_t storage;
    r->done = xSemaphoreCreateBinaryStatic(&storage);
    r->caller = xTaskGetCurrentTaskHandle();
    r->next = NULL;
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xQueueSend(c->req_q, &r, pdMS_TO_TICKS(CONFIG_EUACX_REQ_TIMEOUT_MS)) == pdTRUE) {
        xTaskNotifyGive(c->mgr);
        /* A queued request references caller-owned memory until the reply. */
        xSemaphoreTake(r->done, portMAX_DELAY);
        result = r->result;
    }
    vSemaphoreDelete(r->done);
    return result;
}

BaseType_t euacx_task_create(euacx_context_t *c, TaskFunction_t fn, const char *name,
                             uint32_t stack, void *arg, unsigned priority, TaskHandle_t *task)
{
    return xTaskCreatePinnedToCore(fn, name, stack, arg, priority, task,
                                  c->config.task_core < 0 ? tskNO_AFFINITY : c->config.task_core);
}
