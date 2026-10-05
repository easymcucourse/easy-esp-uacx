/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include "esp_log.h"
#include "euacx_runtime.h"

static bool fill_transfer(euacx_stream_state_t *s, usb_transfer_t *t)
{
    size_t off = 0;
    unsigned frame = s->alt.subslot * 2;
    unsigned base = s->port->info.speed == EUACX_SPEED_HS ? 8000 : 1000;
    bool feedback = s->feedback != NULL;
    for (unsigned i = 0; i < s->packets; ++i) {
        /* Nominal scheduling retains exact 44.1 kHz rational arithmetic. */
        unsigned frames;
        if (feedback) {
            s->phase += (uint64_t)atomic_load(&s->feedback_q16) * euacx_service_ticks(&s->alt);
            frames = s->phase >> 16; s->phase &= 0xffff;
        } else {
            s->phase += (uint64_t)s->cfg.sample_rate * euacx_service_ticks(&s->alt);
            frames = s->phase / base; s->phase %= base;
        }
        size_t bytes = frames * frame;
        if (bytes > s->alt.mps || off + bytes > t->data_buffer_size) { atomic_store(&s->fault, true); return false; }
        bool warming = atomic_load(&s->warming);
        size_t available = warming ? 0 : euacx_ring_used(&s->ring);
        available -= available % frame;
        if (available > bytes) available = bytes;
        size_t n = euacx_ring_take(&s->ring, t->data_buffer + off, available);
        memset(t->data_buffer + off + n, 0, bytes - n);
        if (n < bytes && !warming && !atomic_load(&s->eof)) atomic_fetch_add(&s->underruns, 1);
        t->isoc_packet_desc[i].num_bytes = bytes;
        off += bytes;
    }
    t->num_bytes = off;
    xSemaphoreGive(s->port->space);
    return true;
}
static bool submit(euacx_stream_state_t *s, usb_transfer_t *t)
{
    if (atomic_load(&s->stop) || atomic_load(&s->port->gone)) return false;
    atomic_fetch_add(&s->inflight, 1);
    if (usb_host_transfer_submit(t) == ESP_OK) return true;
    atomic_fetch_sub(&s->inflight, 1);
    atomic_store(&s->fault, true);
    return false;
}
static void iso_process(usb_transfer_t *t)
{
    euacx_stream_state_t *s = t->context;
    bool bad = t->status != USB_TRANSFER_STATUS_COMPLETED;
    if (!bad) for (int i = 0; i < t->num_isoc_packets; ++i)
        if (t->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED) bad = true;
    atomic_fetch_sub(&s->inflight, 1);
    if (atomic_load(&s->stop) || atomic_load(&s->port->gone)) return;
    if (bad) {
        atomic_fetch_add(&s->errors, 1); ++s->consecutive_errors;
        ESP_LOGW("euacx", "ISO completion status=%d consecutive=%u", t->status, s->consecutive_errors);
        for (int i = 0; i < t->num_isoc_packets; ++i)
            if (t->isoc_packet_desc[i].status != USB_TRANSFER_STATUS_COMPLETED)
                ESP_LOGW("euacx", "ISO packet=%d status=%d bytes=%d", i, t->isoc_packet_desc[i].status,
                         t->isoc_packet_desc[i].actual_num_bytes);
    }
    else s->consecutive_errors = 0;
    if (s->consecutive_errors >= CONFIG_EUACX_MAX_ERRORS) atomic_store(&s->fault, true);
    if (atomic_load(&s->fault) || (atomic_load(&s->eof) && !euacx_ring_used(&s->ring))) {
        xTaskNotifyGive(s->port->ctx->mgr); return;
    }
    if (fill_transfer(s, t)) submit(s, t);
    if (atomic_load(&s->fault)) xTaskNotifyGive(s->port->ctx->mgr);
}
static void feedback_process(usb_transfer_t *t)
{
    euacx_stream_state_t *s = t->context;
    atomic_fetch_sub(&s->inflight, 1);
    if (atomic_load(&s->stop) || atomic_load(&s->port->gone) || atomic_load(&s->fault) ||
        (atomic_load(&s->eof) && !euacx_ring_used(&s->ring))) return;
    size_t off = 0;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) for (int i = 0; i < t->num_isoc_packets; ++i) {
        uint32_t rate;
        if (t->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_COMPLETED &&
            euacx_feedback_rate(t->data_buffer + off, t->isoc_packet_desc[i].actual_num_bytes,
                                s->port->info.speed, s->cfg.sample_rate, &rate)) atomic_store(&s->feedback_q16, rate);
        off += t->isoc_packet_desc[i].num_bytes;
    }
    submit(s, t);
}
static void iso_callback(usb_transfer_t *t)
{
    euacx_stream_state_t *s = t->context;
    atomic_fetch_add(&s->refs, 1);
    iso_process(t);
    atomic_fetch_sub(&s->refs, 1);
}
static void feedback_callback(usb_transfer_t *t)
{
    euacx_stream_state_t *s = t->context;
    atomic_fetch_add(&s->refs, 1);
    feedback_process(t);
    atomic_fetch_sub(&s->refs, 1);
}
void euacx_iso_start(euacx_stream_state_t *s)
{
    if (atomic_load(&s->started) || !atomic_load(&s->ready) || atomic_load(&s->stop)) return;
    size_t used = euacx_ring_used(&s->ring);
    if (used < s->ring.size / 2 && !atomic_load(&s->eof)) return;
    if (!used && atomic_load(&s->eof)) { xTaskNotifyGive(s->port->ctx->mgr); return; }
    atomic_store(&s->started, true);
    for (unsigned i = 0; i < CONFIG_EUACX_NUM_TRANSFERS; ++i) {
        usb_transfer_t *t = s->transfers[i];
        t->device_handle = s->port->dev->usb; t->bEndpointAddress = s->alt.ep;
        t->callback = iso_callback; t->context = s;
        if (!fill_transfer(s, t) || !submit(s, t)) break;
    }
    if (s->feedback) {
        usb_transfer_t *t = s->feedback;
        t->device_handle = s->port->dev->usb; t->bEndpointAddress = s->alt.feedback_ep;
        t->callback = feedback_callback; t->context = s;
        t->num_bytes = s->alt.feedback_mps * t->num_isoc_packets;
        for (int i = 0; i < t->num_isoc_packets; ++i) t->isoc_packet_desc[i].num_bytes = s->alt.feedback_mps;
        submit(s, t);
    }
}
void euacx_iso_dispose(euacx_stream_state_t *s)
{
    for (unsigned i = 0; i < CONFIG_EUACX_NUM_TRANSFERS; ++i) usb_host_transfer_free(s->transfers[i]);
    usb_host_transfer_free(s->feedback);
}
