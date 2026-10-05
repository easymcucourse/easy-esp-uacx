/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "euacx_runtime.h"

static void disconnect(euacx_port_t *p)
{
    euacx_dev_t *d = p->dev;
    if (!d || p->stream || atomic_load(&p->ctx->ctrl_pending)) return;
    if (!d->detached && d->driver && d->driver->detach) d->driver->detach(d, d->driver_ctx);
    d->detached = true;
    if (d->claimed >= 0) usb_host_interface_release(p->ctx->client, d->usb, d->claimed);
    esp_err_t e = usb_host_device_close(p->ctx->client, d->usb);
    if (e != ESP_OK) { ESP_LOGE("euacx", "device close deferred: %s", esp_err_to_name(e)); return; }
    euacx_notice_t *n = d->disconnected;
    portENTER_CRITICAL(&p->ctx->lock);
    p->dev = NULL; p->state = EUACX_STATE_DISCONNECTED;
    portEXIT_CRITICAL(&p->ctx->lock);
    if (n) {
        n->id = NOTICE_DISCONNECTED; n->port = p; n->conn = p->info.conn_id;
        euacx_notice_send(p->ctx, n);
    }
    free(d);
}
static void attach(euacx_context_t *c, uint8_t addr)
{
    if (atomic_load(&c->shutting)) return;
    usb_device_handle_t usb;
    if (usb_host_device_open(c->client, addr, &usb) != ESP_OK) return;
    const usb_config_desc_t *config = NULL;
    const usb_device_desc_t *descriptor = NULL;
    usb_device_info_t u = {0};
    euacx_dev_t *d = calloc(1, sizeof(*d));
    esp_err_t e = d ? usb_host_get_active_config_descriptor(usb, &config) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = euacx_parse((const uint8_t *)config, config->wTotalLength, &d->caps);
    if (e == ESP_OK) e = usb_host_get_device_descriptor(usb, &descriptor);
    if (e == ESP_OK) e = usb_host_device_info(usb, &u);
    euacx_port_t *p = &c->port;
    if (e != ESP_OK || p->dev || u.speed == USB_SPEED_LOW) {
        if (p->dev && e == ESP_OK) ESP_LOGW("euacx", "ignore second DAC at address %u", addr);
        free(d); usb_host_device_close(c->client, usb); return;
    }
#ifdef CONFIG_EUACX_DUMP_DESCRIPTORS
    ESP_LOG_BUFFER_HEX_LEVEL("euacx_desc", config, config->wTotalLength, ESP_LOG_INFO);
#endif
    d->usb = usb; d->claimed = -1; d->driver = euacx_driver_find(descriptor->idVendor, descriptor->idProduct);
    euacx_notice_t *connected = calloc(1, sizeof(*connected));
    d->disconnected = calloc(1, sizeof(*d->disconnected));
    if (!connected || !d->disconnected) { free(connected); free(d->disconnected); free(d); usb_host_device_close(c->client, usb); return; }
    portENTER_CRITICAL(&c->lock);
    uint32_t conn = p->info.conn_id + 1;
    memset(&p->info, 0, sizeof(p->info)); p->info.conn_id = conn;
    p->info.vid = descriptor->idVendor; p->info.pid = descriptor->idProduct;
    p->info.speed = u.speed == USB_SPEED_HIGH ? EUACX_SPEED_HS : EUACX_SPEED_FS;
    p->dev = d; p->state = EUACX_STATE_ENUMERATING;
    atomic_store(&p->gone, false);
    portEXIT_CRITICAL(&c->lock);
    /* Device strings are cached UTF-16LE; bounded ASCII fallback for logging. */
    if (u.str_desc_product && u.str_desc_product->bLength >= 2) {
        const uint8_t *str = (const uint8_t *)u.str_desc_product;
        unsigned n = (str[0] - 2) / 2;
        if (n > sizeof(p->info.product) - 1) n = sizeof(p->info.product) - 1;
        for (unsigned i = 0; i < n; ++i) p->info.product[i] = str[2 + 2 * i] < 128 && !str[3 + 2 * i] ? str[2 + 2 * i] : '?';
    }
    if (d->caps.clock_fallback) ESP_LOGW("euacx", "clock selector/multiplier or missing link: using first clock source");
    if (d->driver->attach) e = d->driver->attach(d, &d->driver_ctx);
    if (e == ESP_OK && d->driver->fixup_caps) e = d->driver->fixup_caps(d, d->driver_ctx, &d->caps);
    if (e == ESP_OK) euacx_probe_rates(p);
    /* Claim creates host pipes without changing the device's alternate setting.
     * Probe actual controller/FIFO limits before advertising any capability;
     * USB bus bandwidth alone cannot detect S3's 600-byte periodic OUT limit. */
    for (unsigned i = 0; e == ESP_OK && i < d->caps.num_alts; ++i) {
        euacx_alt_t *a = &d->caps.alts[i];
        esp_err_t claim = usb_host_interface_claim(c->client, d->usb, a->interface, a->alt);
        if (claim == ESP_OK) usb_host_interface_release(c->client, d->usb, a->interface);
        else {
            a->rates.num_rates = 0;
            ESP_LOGW("euacx", "host excludes alt=%u bits=%u MPS=%u: %s", a->alt, a->bits, a->mps, esp_err_to_name(claim));
        }
    }
    if (e == ESP_OK) e = euacx_probe_controls(p);
    if (e == ESP_OK) {
        euacx_build_info(&d->caps, d->driver, CONFIG_EUACX_USE_VERIFIED_CAPS, &p->info);
        if (p->info.verified) {
            const euacx_verified_caps_t *v = d->driver->verified;
            const euacx_verified_pcm_t *list = p->info.speed == EUACX_SPEED_HS ? v->hs : v->fs;
            unsigned n = p->info.speed == EUACX_SPEED_HS ? v->num_hs : v->num_fs;
            for (unsigned b = 0; b < n; ++b) for (unsigned r = 0; r < list[b].rates.num_rates; ++r) {
                euacx_stream_config_t req = {.format = EUACX_FORMAT_PCM, .channels = 2, .bits = list[b].bits, .sample_rate = list[b].rates.rates[r]};
                if (!euacx_select_verified(&d->caps, p->info.speed, &req, d->driver, true)) ESP_LOGW("euacx", "VERIFIED MISMATCH bits=%u rate=%lu", req.bits, (unsigned long)req.sample_rate);
            }
        }
        if (d->driver->verified && d->driver->verified->has_volume && p->info.has_volume) {
            int16_t volume = d->driver->verified->volume_db256;
            e = euacx_hw_volume(p, true, &volume);
        }
    }
    if (e != ESP_OK || atomic_load(&p->gone)) {
        ESP_LOGW("euacx", "enumeration failed: %s", esp_err_to_name(e));
        free(connected); free(d->disconnected); d->disconnected = NULL;
        atomic_store(&p->gone, true); disconnect(p); return;
    }
    portENTER_CRITICAL(&c->lock);
    p->state = EUACX_STATE_CONNECTED;
    portEXIT_CRITICAL(&c->lock);
    connected->id = NOTICE_CONNECTED; connected->port = p; connected->conn = conn;
    euacx_notice_send(c, connected);
    ESP_LOGI("euacx", "connected %04x:%04x driver=%s conn=%lu speed=%s verified=%u",
             p->info.vid, p->info.pid, p->info.driver, (unsigned long)conn,
             p->info.speed == EUACX_SPEED_HS ? "HS" : "FS", p->info.verified);
}

void euacx_usb_event(const usb_host_client_event_msg_t *e, void *arg)
{
    euacx_context_t *c = arg;
    uint8_t addr = 0;
    if (e->event == USB_HOST_CLIENT_EVENT_NEW_DEV && e->new_dev.address < 128) {
        addr = e->new_dev.address; atomic_store(&c->addresses[addr], true);
    } else if (e->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        portENTER_CRITICAL(&c->lock);
        if (c->port.dev && c->port.dev->usb == e->dev_gone.dev_hdl) atomic_store(&c->port.gone, true);
        portEXIT_CRITICAL(&c->lock);
        xSemaphoreGive(c->port.space);
    }
    /* State/address flags are lossless fallback if int_q is full. */
    xQueueSend(c->int_q, &addr, 0); xTaskNotifyGive(c->mgr);
}
void euacx_pump_task(void *arg)
{
    euacx_context_t *c = arg;
    while (!atomic_load(&c->active)) vTaskDelay(1);
    while (!atomic_load(&c->pump_exit)) {
        usb_host_client_handle_events(c->client, pdMS_TO_TICKS(2) + 1);
        portENTER_CRITICAL(&c->lock);
        euacx_stream_state_t *s = c->port.stream;
        if (s && !atomic_load(&s->stop) && !atomic_load(&c->port.gone)) atomic_fetch_add(&s->refs, 1);
        else s = NULL;
        portEXIT_CRITICAL(&c->lock);
        if (s) { euacx_iso_start(s); atomic_fetch_sub(&s->refs, 1); }
    }
    xSemaphoreGive(c->task_done); atomic_store(&c->pump_exited, true); vTaskDelete(NULL);
}
void euacx_library_task(void *arg)
{
    euacx_context_t *c = arg;
    while (!atomic_load(&c->active)) vTaskDelay(1);
    while (!atomic_load(&c->lib_exit)) {
        uint32_t flags;
        usb_host_lib_handle_events(pdMS_TO_TICKS(20) + 1, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
    xSemaphoreGive(c->task_done); atomic_store(&c->lib_exited, true); vTaskDelete(NULL);
}
static bool service(euacx_context_t *c)
{
    uint8_t addr;
    while (xQueueReceive(c->int_q, &addr, 0) == pdTRUE) {}
    euacx_port_t *p = &c->port;
    euacx_stream_state_t *s = p->stream;
    if (s) {
        if (atomic_load(&s->started) && atomic_load(&s->warming) &&
            !atomic_load(&s->stop) && !atomic_load(&p->gone)) {
            esp_err_t e = p->info.has_mute ? euacx_hw_mute(p, true, &s->restore_mute) : ESP_OK;
            if (e != ESP_OK) atomic_store(&s->fault, true);
            atomic_store(&s->warming, false);
        }
        if (atomic_load(&p->gone)) euacx_stream_stop(p, atomic_load(&c->shutting) ? EUACX_STOP_CLOSED : EUACX_STOP_UNPLUGGED, ESP_OK);
        else if (atomic_load(&s->fault)) euacx_stream_stop(p, EUACX_STOP_ERROR, ESP_FAIL);
        else if (s->cfg.on_data && atomic_load(&s->abort)) euacx_stream_stop(p, EUACX_STOP_CLOSED, ESP_OK);
        else if (atomic_load(&s->eof) && !euacx_ring_used(&s->ring) && !atomic_load(&s->inflight)) euacx_stream_stop(p, EUACX_STOP_EOF, ESP_OK);
        euacx_stream_finish(p);
    }
    if (atomic_load(&p->gone)) disconnect(p);
    for (unsigned i = 1; i < 128; ++i) if (atomic_exchange(&c->addresses[i], false)) attach(c, i);
    return !p->dev && !atomic_load(&c->ctrl_pending);
}
static void dispatch(euacx_context_t *c, euacx_request_t *r)
{
    euacx_port_t *p = &c->port;
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (r->id == REQ_DEINIT) {
        if (c->deinit) { euacx_request_reply(r, e); return; }
        atomic_store(&c->shutting, true); c->deinit = r;
        euacx_stream_stop(p, EUACX_STOP_CLOSED, ESP_OK);
        atomic_store(&p->gone, true); xSemaphoreGive(p->space);
        return;
    }
    if (atomic_load(&c->shutting)) { euacx_request_reply(r, e); return; }
    if (r->id == REQ_CLOSE) {
        euacx_stream_state_t *s = p->stream;
        if (!s) e = ESP_OK;
        else if (r->caller == s->feed || (!atomic_load(&p->gone) && !s->cfg.on_data && r->caller != s->owner)) e = ESP_ERR_INVALID_STATE;
        else {
            r->next = s->close_requests; s->close_requests = r;
            euacx_stream_stop(p, EUACX_STOP_CLOSED, ESP_OK); return;
        }
    } else if (p->state == EUACX_STATE_CONNECTED || p->state == EUACX_STATE_STREAMING) {
        if (!atomic_load(&p->gone)) switch (r->id) {
            case REQ_OPEN: e = euacx_stream_start(p, r); break;
            case REQ_VOLUME_SET: e = euacx_hw_volume(p, true, &r->volume); break;
            case REQ_VOLUME_GET: e = euacx_hw_volume(p, false, r->out); break;
            case REQ_MUTE_SET:
                if (p->stream && atomic_load(&p->stream->warming)) p->stream->restore_mute = r->mute;
                e = euacx_hw_mute(p, true, &r->mute); break;
            case REQ_MUTE_GET: e = euacx_hw_mute(p, false, r->out); break;
            default: break;
        }
    }
    /* Observe disconnect during an OPEN/control transaction before replying. */
    if (atomic_load(&p->gone) && r->id != REQ_CLOSE) e = ESP_ERR_INVALID_STATE;
    euacx_request_reply(r, e);
}
void euacx_manager_task(void *arg)
{
    euacx_context_t *c = arg;
    while (!atomic_load(&c->active)) vTaskDelay(1);
    TickType_t recovery_at = xTaskGetTickCount();
    TickType_t stats_at = recovery_at;
    for (;;) {
        bool empty = service(c);
        euacx_request_t *r;
        if (xQueueReceive(c->req_q, &r, 0) == pdTRUE) { service(c); dispatch(c, r); continue; }
        if (c->deinit && empty && atomic_load(&c->api_users) == 1) break;
        if (c->port.stream && xTaskGetTickCount() - stats_at >= pdMS_TO_TICKS(30000)) {
            euacx_stream_state_t *s = c->port.stream;
            stats_at = xTaskGetTickCount();
            ESP_LOGI("euacx", "STATS errors=%lu underruns=%lu buffered=%lu inflight=%lu",
                (unsigned long)atomic_load(&s->errors), (unsigned long)atomic_load(&s->underruns),
                (unsigned long)euacx_ring_used(&s->ring), (unsigned long)atomic_load(&s->inflight));
        }
        if (c->installed && !c->port.dev && !atomic_load(&c->shutting) && CONFIG_EUACX_RECOVERY_MS &&
            xTaskGetTickCount() - recovery_at >= pdMS_TO_TICKS(CONFIG_EUACX_RECOVERY_MS)) {
            recovery_at = xTaskGetTickCount();
            ESP_LOGW("euacx", "no DAC: retry root/Hub enumeration");
            if (usb_host_lib_set_root_port_power(false) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(100) + 1);
                usb_host_lib_set_root_port_power(true);
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2) + 1);
    }
    atomic_store(&c->pump_exit, true); usb_host_client_unblock(c->client);
    while (!atomic_load(&c->pump_exited)) xSemaphoreTake(c->task_done, pdMS_TO_TICKS(10) + 1);
    usb_host_client_deregister(c->client);
    atomic_store(&c->cb_exit, true); xSemaphoreGive(c->cb_wake);
    /* deinit invoked from a callback must return so that callback can exit. */
    if (c->deinit->caller != c->cb)
        while (!atomic_load(&c->cb_exited)) xSemaphoreTake(c->task_done, pdMS_TO_TICKS(10) + 1);
    if (c->installed) {
        usb_host_lib_set_root_port_power(false);
        usb_host_device_free_all();
        usb_host_lib_info_t info;
        do { usb_host_lib_info(&info); if (info.num_devices) vTaskDelay(pdMS_TO_TICKS(10) + 1); } while (info.num_devices);
        atomic_store(&c->lib_exit, true); usb_host_lib_unblock();
        while (!atomic_load(&c->lib_exited)) xSemaphoreTake(c->task_done, pdMS_TO_TICKS(10) + 1);
        /* Uninstall also requires every pending library event flag to be
         * consumed. num_devices==0 alone does not satisfy that contract. */
        while (usb_host_uninstall() != ESP_OK) {
            uint32_t flags;
            usb_host_lib_handle_events(pdMS_TO_TICKS(10) + 1, &flags);
            if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
        }
    }
    euacx_request_reply(c->deinit, ESP_OK);
    vTaskDelete(NULL);
}
