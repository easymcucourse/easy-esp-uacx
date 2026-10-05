/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include "esp_intr_alloc.h"
#include "euacx_runtime.h"

static portMUX_TYPE lifecycle = portMUX_INITIALIZER_UNLOCKED;
static euacx_context_t *current;
static bool initializing;
static euacx_context_t *acquire(euacx_port_t *p, bool check_port)
{
    portENTER_CRITICAL(&lifecycle);
    euacx_context_t *c = current;
    if (!c || atomic_load(&c->shutting) || (check_port && p != &c->port)) c = NULL;
    if (c) atomic_fetch_add(&c->api_users, 1);
    portEXIT_CRITICAL(&lifecycle);
    return c;
}
static void release(euacx_context_t *c) { atomic_fetch_sub(&c->api_users, 1); }
void euacx_context_destroy(euacx_context_t *c)
{
    portENTER_CRITICAL(&lifecycle);
    if (current == c) current = NULL;
    initializing = true;
    portEXIT_CRITICAL(&lifecycle);
    vQueueDelete(c->req_q); vQueueDelete(c->int_q); vQueueDelete(c->cb_q);
    vSemaphoreDelete(c->cb_wake); vSemaphoreDelete(c->task_done);
    vSemaphoreDelete(c->port.space);
    free(c);
    portENTER_CRITICAL(&lifecycle); initializing = false; portEXIT_CRITICAL(&lifecycle);
}
esp_err_t euacx_init(const euacx_config_t *config)
{
    if (!config || config->task_priority < 1 || config->task_priority >= CONFIG_EUACX_PUMP_PRIORITY ||
        config->task_core < -1 || config->task_core >= portNUM_PROCESSORS ||
        CONFIG_EUACX_FEED_PRIORITY >= CONFIG_EUACX_PUMP_PRIORITY) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&lifecycle);
    bool busy = current || initializing;
    if (!busy) initializing = true;
    portEXIT_CRITICAL(&lifecycle);
    if (busy) return ESP_ERR_INVALID_STATE;
    euacx_context_t *c = calloc(1, sizeof(*c));
    esp_err_t e = c ? ESP_OK : ESP_ERR_NO_MEM;
    if (!c) goto done;
    c->config = *config; c->port.ctx = c;
    c->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    c->req_q = xQueueCreate(CONFIG_EUACX_REQ_QUEUE_LEN, sizeof(euacx_request_t *));
    c->int_q = xQueueCreate(CONFIG_EUACX_INT_QUEUE_LEN, sizeof(uint8_t));
    c->cb_q = xQueueCreate(CONFIG_EUACX_CB_QUEUE_LEN, sizeof(euacx_notice_t *));
    c->cb_wake = xSemaphoreCreateBinary();
    c->task_done = xSemaphoreCreateCounting(4, 0); c->port.space = xSemaphoreCreateBinary();
    if (!c->req_q || !c->int_q || !c->cb_q || !c->cb_wake || !c->task_done || !c->port.space) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK && config->install_usb_host) {
        usb_host_config_t host = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
        e = usb_host_install(&host); c->installed = e == ESP_OK;
    }
    if (e == ESP_OK) {
        usb_host_client_config_t client = { .is_synchronous = false, .max_num_event_msg = CONFIG_EUACX_INT_QUEUE_LEN,
            .async = { .client_event_callback = euacx_usb_event, .callback_arg = c } };
        e = usb_host_client_register(&client, &c->client);
    }
    if (e == ESP_OK && euacx_task_create(c, euacx_callback_task, "euacx_cb", CONFIG_EUACX_CB_STACK, c, config->task_priority, &c->cb) != pdPASS) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK && euacx_task_create(c, euacx_pump_task, "euacx_pump", CONFIG_EUACX_PUMP_STACK, c, CONFIG_EUACX_PUMP_PRIORITY, &c->pump) != pdPASS) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK && c->installed && euacx_task_create(c, euacx_library_task, "usb_lib", 4096, c, CONFIG_EUACX_LIB_PRIORITY, &c->lib) != pdPASS) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK && euacx_task_create(c, euacx_manager_task, "euacx_mgr", CONFIG_EUACX_TASK_STACK, c, config->task_priority, &c->mgr) != pdPASS) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        portENTER_CRITICAL(&lifecycle); current = c; initializing = false; portEXIT_CRITICAL(&lifecycle);
        atomic_store(&c->active, true);
        return ESP_OK;
    }
    /* All created tasks are still behind the activation gate. */
    if (c->cb) vTaskDelete(c->cb);
    if (c->pump) vTaskDelete(c->pump);
    if (c->lib) vTaskDelete(c->lib);
    if (c->mgr) vTaskDelete(c->mgr);
    if (c->client) usb_host_client_deregister(c->client);
    if (c->installed) {
        usb_host_device_free_all();
        uint32_t flags;
        while (usb_host_uninstall() != ESP_OK) usb_host_lib_handle_events(1, &flags);
    }
    if (c->req_q) vQueueDelete(c->req_q);
    if (c->int_q) vQueueDelete(c->int_q);
    if (c->cb_q) vQueueDelete(c->cb_q);
    if (c->cb_wake) vSemaphoreDelete(c->cb_wake);
    if (c->task_done) vSemaphoreDelete(c->task_done);
    if (c->port.space) vSemaphoreDelete(c->port.space);
    free(c);
done:
    portENTER_CRITICAL(&lifecycle); initializing = false; portEXIT_CRITICAL(&lifecycle);
    return e;
}
esp_err_t euacx_deinit(void)
{
    euacx_context_t *c = acquire(NULL, false);
    if (!c) return ESP_ERR_INVALID_STATE;
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&c->lock);
    bool feed = c->port.stream && c->port.stream->feed == caller;
    portEXIT_CRITICAL(&c->lock);
    if (feed || caller == c->mgr || caller == c->pump) { release(c); return ESP_ERR_INVALID_STATE; }
    euacx_request_t r = { .id = REQ_DEINIT };
    esp_err_t e = euacx_request_call(c, &r);
    if (e == ESP_OK) {
        if (caller == c->cb) {
            /* Free only after the callback returns and its task drains notices. */
            c->deferred_free = true; release(c);
        } else { release(c); euacx_context_destroy(c); }
    } else release(c);
    return e;
}
int euacx_port_count(void)
{
    euacx_context_t *c = acquire(NULL, false);
    if (!c) return 0;
    release(c); return 1;
}
euacx_port_t *euacx_get_port(int index)
{
    euacx_context_t *c = acquire(NULL, false);
    if (!c) return NULL;
    euacx_port_t *p = index == 0 ? &c->port : NULL; release(c); return p;
}
euacx_state_t euacx_get_state(euacx_port_t *p)
{
    euacx_context_t *c = acquire(p, true);
    if (!c) return EUACX_STATE_DISCONNECTED;
    portENTER_CRITICAL(&c->lock);
    euacx_state_t state = atomic_load(&p->gone) ? EUACX_STATE_DISCONNECTED : p->state;
    portEXIT_CRITICAL(&c->lock); release(c); return state;
}
esp_err_t euacx_get_info(euacx_port_t *p, euacx_info_t *out)
{
    if (!out || !p) return ESP_ERR_INVALID_ARG;
    euacx_context_t *c = acquire(p, true);
    if (!c) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&c->lock);
    bool valid = !atomic_load(&p->gone) && (p->state == EUACX_STATE_CONNECTED || p->state == EUACX_STATE_STREAMING);
    if (valid) *out = p->info;
    portEXIT_CRITICAL(&c->lock); release(c); return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
static esp_err_t request(euacx_port_t *p, euacx_request_t *r)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    euacx_context_t *c = acquire(p, true);
    if (!c) return ESP_ERR_INVALID_STATE;
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&c->lock);
    euacx_stream_state_t *s = p->stream;
    bool denied = s && ((r->id == REQ_OPEN || r->id == REQ_CLOSE) && caller == s->feed);
    if (r->id == REQ_CLOSE && s && !atomic_load(&p->gone) &&
        euacx_state_check(p->state, EUACX_OP_CLOSE, s->cfg.on_data != NULL, caller == s->owner, caller == s->feed, atomic_load(&s->abort)) != ESP_OK) denied = true;
    portEXIT_CRITICAL(&c->lock);
    r->port = p;
    esp_err_t e = denied ? ESP_ERR_INVALID_STATE : euacx_request_call(c, r);
    if (e == ESP_OK && r->id == REQ_OPEN) {
        portENTER_CRITICAL(&c->lock);
        if (p->stream && p->stream->owner == caller) atomic_store(&p->stream->ready, true);
        portEXIT_CRITICAL(&c->lock);
    }
    release(c); return e;
}
esp_err_t euacx_stream_open(euacx_port_t *p, const euacx_stream_config_t *cfg, euacx_mode_t *mode)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    euacx_request_t r = {.id = REQ_OPEN, .cfg = *cfg, .out = mode}; return request(p, &r);
}
esp_err_t euacx_stream_close(euacx_port_t *p)
{
    euacx_request_t r = {.id = REQ_CLOSE}; return request(p, &r);
}
esp_err_t euacx_write(euacx_port_t *p, const void *data, size_t len, size_t *written, uint32_t timeout_ms)
{
    if (written) *written = 0;
    if (!p || (!data && len)) return ESP_ERR_INVALID_ARG;
    euacx_context_t *c = acquire(p, true);
    if (!c) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&c->lock);
    euacx_stream_state_t *s = p->stream;
    if (s && euacx_state_check(p->state, EUACX_OP_WRITE, s->cfg.on_data != NULL,
        s->owner == xTaskGetCurrentTaskHandle(), s->feed == xTaskGetCurrentTaskHandle(), atomic_load(&s->abort)) == ESP_OK &&
        !atomic_load(&s->stop) && !atomic_load(&p->gone)) atomic_fetch_add(&s->refs, 1);
    else s = NULL;
    portEXIT_CRITICAL(&c->lock);
    /* pdMS_TO_TICKS multiplies in TickType_t and overflows for long waits. */
    uint64_t ticks = (uint64_t)timeout_ms * configTICK_RATE_HZ / 1000;
    TickType_t wait = timeout_ms == UINT32_MAX ? portMAX_DELAY :
        (ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)ticks);
    if (timeout_ms && !wait) wait = 1;
    esp_err_t e = s ? euacx_stream_write(s, data, len, written, wait) : ESP_ERR_INVALID_STATE;
    if (s) atomic_fetch_sub(&s->refs, 1);
    release(c); return e;
}
esp_err_t euacx_stream_abort(euacx_port_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    euacx_context_t *c = acquire(p, true);
    if (!c) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&c->lock);
    if (p->stream) atomic_store(&p->stream->abort, true);
    portEXIT_CRITICAL(&c->lock);
    xSemaphoreGive(p->space); xTaskNotifyGive(c->mgr); release(c); return ESP_OK;
}
esp_err_t euacx_set_volume(euacx_port_t *p, int16_t value)
{
    euacx_request_t r = {.id = REQ_VOLUME_SET, .volume = value}; return request(p, &r);
}
esp_err_t euacx_get_volume(euacx_port_t *p, int16_t *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    *value = 0; euacx_request_t r = {.id = REQ_VOLUME_GET, .out = value}; return request(p, &r);
}
esp_err_t euacx_set_mute(euacx_port_t *p, bool value)
{
    euacx_request_t r = {.id = REQ_MUTE_SET, .mute = value}; return request(p, &r);
}
esp_err_t euacx_get_mute(euacx_port_t *p, bool *value)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    *value = false; euacx_request_t r = {.id = REQ_MUTE_GET, .out = value}; return request(p, &r);
}
