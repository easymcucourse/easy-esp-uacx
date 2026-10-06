/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "euacx_runtime.h"

esp_err_t euacx_stream_write(euacx_stream_state_t *s, const void *data, size_t len,
                             size_t *written, TickType_t wait)
{
    size_t done = 0;
    if (written) *written = 0;
    TickType_t start = xTaskGetTickCount();
    unsigned frame = s->cfg.bits / 8 * 2, wire_frame = s->alt.subslot * 2;
    const uint8_t *src = data;
    uint8_t wire[512];
    esp_err_t result = ESP_OK;
    /* Exactly one producer per stream. Mutex also prevents concurrent disposal. */
    if (xSemaphoreTake(s->writer, wait) != pdTRUE) return ESP_ERR_TIMEOUT;
    while (done < len) {
        if (atomic_load(&s->stop) || atomic_load(&s->abort) || atomic_load(&s->port->gone)) { result = ESP_ERR_INVALID_STATE; break; }
        if (euacx_ring_space(&s->ring) >= wire_frame) {
            if (!s->partial_size && len - done >= frame) {
                size_t frames = euacx_ring_space(&s->ring) / wire_frame;
                if (frames > (len - done) / frame) frames = (len - done) / frame;
                if (s->cfg.bits == s->alt.subslot * 8) {
                    euacx_ring_put(&s->ring, src + done, frames * frame);
                } else {
                    if (frames > sizeof(wire) / wire_frame) frames = sizeof(wire) / wire_frame;
                    euacx_pcm_convert(src + done, frames * 2, s->cfg.bits, s->alt.subslot, wire);
                    euacx_ring_put(&s->ring, wire, frames * wire_frame);
                }
                done += frames * frame;
                continue;
            }
            size_t n = frame - s->partial_size;
            if (n > len - done) n = len - done;
            memcpy(s->partial + s->partial_size, src + done, n);
            done += n; s->partial_size += n;
            if (s->partial_size == frame) {
                euacx_pcm_convert(s->partial, 2, s->cfg.bits, s->alt.subslot, wire);
                euacx_ring_put(&s->ring, wire, wire_frame);
                s->partial_size = 0;
            }
            continue;
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (wait != portMAX_DELAY && elapsed >= wait) { result = ESP_ERR_TIMEOUT; break; }
        /* Do not hold any lock while waiting for USB to release ring space. */
        xSemaphoreGive(s->writer);
        TickType_t remaining = wait == portMAX_DELAY ? pdMS_TO_TICKS(10) : wait - elapsed;
        if (remaining > pdMS_TO_TICKS(10)) remaining = pdMS_TO_TICKS(10);
        if (!remaining) remaining = 1;
        xSemaphoreTake(s->port->space, remaining);
        xSemaphoreTake(s->writer, portMAX_DELAY);
    }
    xSemaphoreGive(s->writer);
    if (written) *written = done;
    return result;
}

esp_err_t euacx_stream_start(euacx_port_t *p, euacx_request_t *r)
{
    if (r->cfg.format != EUACX_FORMAT_PCM) return ESP_ERR_NOT_SUPPORTED;
    if (r->cfg.channels != 2 || (r->cfg.bits != 16 && r->cfg.bits != 24 && r->cfg.bits != 32) || !r->cfg.sample_rate) return ESP_ERR_INVALID_ARG;
    if (p->state != EUACX_STATE_CONNECTED || atomic_load(&p->gone) || p->stream) return ESP_ERR_INVALID_STATE;
    const euacx_rate_list_t *rates = &p->info.pcm[r->cfg.bits / 8 - 2];
    bool supported = false;
    for (unsigned i = 0; i < rates->num_rates; ++i) if (rates->rates[i] == r->cfg.sample_rate) supported = true;
    const euacx_alt_t *alt = euacx_select_verified(&p->dev->caps, p->info.speed, &r->cfg,
                                                  p->dev->driver, p->info.verified);
    if (!supported || !alt) return ESP_ERR_NOT_SUPPORTED;
    euacx_stream_state_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->port = p; s->cfg = r->cfg; s->alt = *alt; s->owner = r->caller;
    s->stopped = calloc(1, sizeof(*s->stopped));
    s->writer = xSemaphoreCreateMutex();
    esp_err_t e = (!s->stopped || !s->writer) ? ESP_ERR_NO_MEM : ESP_OK;
    unsigned base = p->info.speed == EUACX_SPEED_HS ? 8000 : 1000;
    atomic_store(&s->feed_exited, r->cfg.on_data == NULL);
    atomic_store(&s->feedback_q16, ((uint64_t)r->cfg.sample_rate << 16) / base);
    atomic_store(&s->warming, true);
    bool muted = false;
    if (e == ESP_OK && p->info.has_mute) {
        esp_err_t mute_error = euacx_hw_mute(p, false, &s->restore_mute);
        bool on = true;
        if (mute_error == ESP_OK) { s->restore_mute_valid = true; mute_error = euacx_hw_mute(p, true, &on); muted = mute_error == ESP_OK; }
        if (mute_error != ESP_OK) ESP_LOGW("euacx", "startup mute unavailable: %s", esp_err_to_name(mute_error));
    }
    if (e == ESP_OK) e = euacx_prepare_stream(p, &r->cfg, &s->alt);
    alt = &s->alt;
    if (e == ESP_OK) {
        uint64_t bytes = (uint64_t)r->cfg.sample_rate * 2 * alt->subslot * CONFIG_EUACX_BUFFER_MS / 1000;
        s->packets = p->info.speed == EUACX_SPEED_HS ? 32 : 8;
        uint64_t max_frames = ((uint64_t)r->cfg.sample_rate * euacx_service_ticks(alt) + base - 1) / base;
        if (alt->feedback_ep) ++max_frames;
        uint64_t pipeline = max_frames * 2 * alt->subslot * s->packets * CONFIG_EUACX_NUM_TRANSFERS;
        if (bytes < 2 * pipeline) bytes = 2 * pipeline;
        s->ring.size = 256;
        while (s->ring.size < bytes && s->ring.size < (1u << 23)) s->ring.size <<= 1;
        s->ring.data = malloc(s->ring.size);
        if (!s->ring.data || bytes > s->ring.size) e = ESP_ERR_NO_MEM;
    }
    for (unsigned i = 0; e == ESP_OK && i < CONFIG_EUACX_NUM_TRANSFERS; ++i)
        e = usb_host_transfer_alloc((size_t)alt->mps * s->packets, s->packets, &s->transfers[i]);
    if (e == ESP_OK && alt->feedback_ep)
        e = usb_host_transfer_alloc(alt->feedback_mps * 8u, 8, &s->feedback);
    bool driver_started = false;
    if (e == ESP_OK && p->dev->driver->stream_start) {
        e = p->dev->driver->stream_start(p->dev, p->dev->driver_ctx, s);
        driver_started = e == ESP_OK;
    }
    if (e == ESP_OK && r->cfg.on_data && euacx_task_create(p->ctx, euacx_feed_task, "euacx_feed", CONFIG_EUACX_FEED_STACK, s,
                                                        CONFIG_EUACX_FEED_PRIORITY, &s->feed) != pdPASS) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK && !atomic_load(&p->gone)) {
        portENTER_CRITICAL(&p->ctx->lock);
        p->stream = s; p->state = EUACX_STATE_STREAMING;
        portEXIT_CRITICAL(&p->ctx->lock);
        if (r->out) *(euacx_mode_t *)r->out = EUACX_MODE_PCM;
        ESP_LOGI("euacx", "stream PCM input=%u rate=%lu alt=%u wire_bits=%u subslot=%u feedback=%u ring=%lu",
                 s->cfg.bits, (unsigned long)s->cfg.sample_rate, s->alt.alt, s->alt.bits, s->alt.subslot,
                 s->alt.feedback_ep, (unsigned long)s->ring.size);
        return ESP_OK;
    }
    /* No feed task survives a failed publication; wait for its start gate. */
    atomic_store(&s->stop, true);
    if (s->feed) {
        while (!atomic_load(&s->feed_exited)) vTaskDelay(1);
    }
    if (driver_started && p->dev->driver->stream_stop) p->dev->driver->stream_stop(p->dev, p->dev->driver_ctx);
    if (p->dev->claimed >= 0) {
        if (!atomic_load(&p->gone)) euacx_set_interface(p, alt->interface, 0);
        usb_host_interface_release(p->ctx->client, p->dev->usb, p->dev->claimed); p->dev->claimed = -1;
    }
    if (muted && !atomic_load(&p->gone)) euacx_hw_mute(p, true, &s->restore_mute);
    euacx_iso_dispose(s);
    if (s->writer) vSemaphoreDelete(s->writer);
    free(s->stopped); free(s->ring.data); free(s);
    return e == ESP_OK ? ESP_ERR_INVALID_STATE : e;
}

void euacx_stream_stop(euacx_port_t *p, euacx_stop_reason_t reason, esp_err_t error)
{
    euacx_stream_state_t *s = p->stream;
    if (!s) return;
    if (s->closing) {
        if (reason == EUACX_STOP_UNPLUGGED) s->reason = reason;
        return;
    }
    s->closing = true; s->reason = reason; s->error = error;
    atomic_store(&s->stop, true);
    xSemaphoreGive(p->space);
    if (atomic_load(&s->started)) {
        usb_host_endpoint_halt(p->dev->usb, s->alt.ep);
        usb_host_endpoint_flush(p->dev->usb, s->alt.ep);
        if (s->feedback) { usb_host_endpoint_halt(p->dev->usb, s->alt.feedback_ep); usb_host_endpoint_flush(p->dev->usb, s->alt.feedback_ep); }
    }
}
bool euacx_stream_finish(euacx_port_t *p)
{
    euacx_stream_state_t *s = p->stream;
    if (!s || !s->closing || atomic_load(&s->inflight) || atomic_load(&s->refs) || !atomic_load(&s->feed_exited)) return false;
    if (!atomic_load(&p->gone)) {
        bool previous = s->restore_mute, on = true, can_restore = s->restore_mute_valid;
        if (p->info.has_mute) {
            if (!atomic_load(&s->warming) && euacx_hw_mute(p, false, &previous) == ESP_OK) can_restore = true;
            if (can_restore) euacx_hw_mute(p, true, &on);
        }
        euacx_set_interface(p, s->alt.interface, 0);
        if (p->info.has_mute && can_restore && !atomic_load(&p->gone)) euacx_hw_mute(p, true, &previous);
    }
    if (p->dev->driver->stream_stop) p->dev->driver->stream_stop(p->dev, p->dev->driver_ctx);
    if (p->dev->claimed >= 0) {
        usb_host_interface_release(p->ctx->client, p->dev->usb, p->dev->claimed);
        p->dev->claimed = -1;
    }
    euacx_iso_dispose(s);
    euacx_notice_t *n = s->stopped;
    n->id = NOTICE_STOPPED; n->port = p; n->conn = p->info.conn_id; n->reason = s->reason;
    n->error = s->reason == EUACX_STOP_ERROR ? s->error : ESP_OK;
    euacx_request_t *requests = s->close_requests;
    portENTER_CRITICAL(&p->ctx->lock);
    p->stream = NULL;
    p->state = atomic_load(&p->gone) ? EUACX_STATE_DISCONNECTED : EUACX_STATE_CONNECTED;
    portEXIT_CRITICAL(&p->ctx->lock);
    ESP_LOGI("euacx", "stream stopped reason=%d errors=%lu underruns=%lu", n->reason,
             (unsigned long)atomic_load(&s->errors), (unsigned long)atomic_load(&s->underruns));
    vSemaphoreDelete(s->writer); free(s->ring.data); free(s);
    euacx_notice_send(p->ctx, n);
    while (requests) { euacx_request_t *next = requests->next; euacx_request_reply(requests, ESP_OK); requests = next; }
    return true;
}
