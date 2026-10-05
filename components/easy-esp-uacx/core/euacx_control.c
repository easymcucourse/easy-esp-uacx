/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "euacx_runtime.h"

typedef struct {
    euacx_context_t *ctx;
    usb_transfer_t *transfer;
    SemaphoreHandle_t done;
    _Atomic unsigned refs;
} control_job_t;
static void control_release(control_job_t *j)
{
    if (atomic_fetch_sub(&j->refs, 1) != 1) return;
    usb_host_transfer_free(j->transfer);
    vSemaphoreDelete(j->done);
    free(j);
}
static void control_cb(usb_transfer_t *t)
{
    control_job_t *j = t->context;
    xSemaphoreGive(j->done);
    atomic_fetch_sub(&j->ctx->ctrl_pending, 1);
    control_release(j);
}
esp_err_t euacx_control(euacx_port_t *p, uint8_t type, uint8_t req,
                        uint16_t val, uint16_t index, void *data, uint16_t size)
{
    if (atomic_load(&p->gone) || !p->dev) return ESP_ERR_INVALID_STATE;
    control_job_t *j = calloc(1, sizeof(*j));
    if (!j) return ESP_ERR_NO_MEM;
    j->ctx = p->ctx; j->done = xSemaphoreCreateBinary();
    esp_err_t e = j->done ? usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + size, 0, &j->transfer) : ESP_ERR_NO_MEM;
    if (e != ESP_OK) { if (j->done) vSemaphoreDelete(j->done); free(j); return e; }
    usb_transfer_t *t = j->transfer;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = type; s->bRequest = req; s->wValue = val; s->wIndex = index; s->wLength = size;
    if (!(type & 0x80) && size) memcpy(t->data_buffer + sizeof(*s), data, size);
    t->num_bytes = sizeof(*s) + size;
    t->device_handle = p->dev->usb; t->callback = control_cb; t->context = j; t->timeout_ms = 1000;
    atomic_init(&j->refs, 2); atomic_fetch_add(&p->ctx->ctrl_pending, 1);
    e = usb_host_transfer_submit_control(p->ctx->client, t);
    if (e != ESP_OK) {
        if (e == ESP_ERR_INVALID_STATE || e == ESP_ERR_NOT_FOUND) { atomic_store(&p->gone, true); xSemaphoreGive(p->space); }
        atomic_fetch_sub(&p->ctx->ctrl_pending, 1); control_release(j);
    }
    else if (xSemaphoreTake(j->done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        /* USB Host 1.4.1 does not implement transfer timeout_ms. The heap job
         * remains owned by the pending callback, never by a returned stack. */
        e = ESP_ERR_TIMEOUT; atomic_store(&p->gone, true); xSemaphoreGive(p->space);
        if (p->ctx->installed) {
            usb_host_lib_set_root_port_power(false);
            usb_host_lib_set_root_port_power(true);
        }
        ESP_LOGE("euacx", "control timeout; recover root port and defer device disposal");
    } else if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        e = ESP_FAIL;
        if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) { atomic_store(&p->gone, true); xSemaphoreGive(p->space); }
    }
    else if (t->actual_num_bytes < (int)(sizeof(*s) + size)) e = ESP_ERR_INVALID_SIZE;
    else if ((type & 0x80) && size) memcpy(data, t->data_buffer + sizeof(*s), size);
    control_release(j);
    return atomic_load(&p->gone) && e == ESP_OK ? ESP_ERR_INVALID_STATE : e;
}
esp_err_t euacx_set_interface(euacx_port_t *p, uint8_t interface, uint8_t alt)
{
    esp_err_t e = euacx_control(p, 1, 0x0b, alt, interface, NULL, 0);
    uint8_t actual = 0xff;
    if (e == ESP_OK) e = euacx_control(p, 0x81, 0x0a, 0, interface, &actual, 1);
    return e == ESP_OK && actual != alt ? ESP_ERR_INVALID_RESPONSE : e;
}
esp_err_t euacx_clock_rate(euacx_port_t *p, const euacx_alt_t *a, uint32_t hz)
{
    uint8_t data[4] = {hz, hz >> 8, hz >> 16, hz >> 24};
    uint16_t entity = (a->clock << 8) | p->dev->caps.ac_interface;
    esp_err_t e = euacx_control(p, 0x21, 1, 0x0100, entity, data, 4);
    if (e == ESP_OK) e = euacx_control(p, 0xa1, 1, 0x0100, entity, data, 4);
    uint32_t actual = data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    if (e == ESP_OK && actual != hz) e = ESP_ERR_NOT_SUPPORTED;
    if (e == ESP_OK && (p->dev->caps.clock_controls[a->clock] & 4)) {
        uint8_t valid = 0;
        e = euacx_control(p, 0xa1, 1, 0x0200, entity, &valid, 1);
        if (e == ESP_OK && !valid) e = ESP_ERR_INVALID_STATE;
    }
    return e;
}
void euacx_probe_rates(euacx_port_t *p)
{
    euacx_dev_caps_t *d = &p->dev->caps;
    for (unsigned a = 0; a < d->num_alts; ++a) {
        euacx_alt_t *alt = &d->alts[a];
        bool cached = false;
        for (unsigned b = 0; b < a; ++b) if (d->alts[b].clock == alt->clock) { alt->rates = d->alts[b].rates; cached = true; break; }
        if (cached) continue;
        uint16_t entity = (alt->clock << 8) | d->ac_interface;
        uint8_t n[2] = {0};
        esp_err_t e = euacx_control(p, 0xa1, 2, 0x0100, entity, n, 2);
        unsigned count = n[0] | (unsigned)n[1] << 8;
        if (e == ESP_OK && count && count <= 32) {
            uint8_t data[2 + 32 * 12];
            e = euacx_control(p, 0xa1, 2, 0x0100, entity, data, 2 + count * 12);
            if (e == ESP_OK) e = euacx_parse_rates(data, 2 + count * 12, &alt->rates);
        } else e = ESP_ERR_NOT_SUPPORTED;
        if (e != ESP_OK) {
            euacx_standard_rates(&alt->rates);
            ESP_LOGW("euacx", "clock %u GET RANGE unavailable; SET/GET will validate open", alt->clock);
        }
    }
}
static unsigned first_channel(uint32_t mask)
{
    for (unsigned i = 0; i < 3; ++i) if (mask & (1u << i)) return i;
    return 32;
}
esp_err_t euacx_probe_controls(euacx_port_t *p)
{
    euacx_dev_t *d = p->dev;
    d->feature = euacx_feature(&d->caps, d->caps.alts[0].terminal);
    if (!d->feature) return ESP_OK;
    const euacx_feature_t *f = d->feature;
    euacx_info_t *info = &p->info;
    info->has_mute = (f->mute_read & f->mute_write & 7) != 0;
    info->has_volume = (f->volume_read & f->volume_write & 7) != 0;
    if (info->has_volume) {
        uint8_t range[8];
        unsigned ch = first_channel(f->volume_read & f->volume_write);
        esp_err_t e = euacx_control(p, 0xa1, 2, 0x0200 | ch,
                                   (f->id << 8) | d->caps.ac_interface, range, sizeof(range));
        if (e != ESP_OK || (range[0] | (unsigned)range[1] << 8) != 1) info->has_volume = false;
        else {
            info->volume_min = (int16_t)(range[2] | (unsigned)range[3] << 8);
            info->volume_max = (int16_t)(range[4] | (unsigned)range[5] << 8);
            info->volume_res = (int16_t)(range[6] | (unsigned)range[7] << 8);
            if (info->volume_min > info->volume_max || info->volume_res < 0) info->has_volume = false;
        }
    }
    bool mute = false;
    if (info->has_mute) return euacx_hw_mute(p, true, &mute);
    return ESP_OK;
}
esp_err_t euacx_hw_volume(euacx_port_t *p, bool set, int16_t *value)
{
    if (!p->info.has_volume || !p->dev->feature) return ESP_ERR_NOT_SUPPORTED;
    const euacx_feature_t *f = p->dev->feature;
    uint32_t mask = f->volume_read & f->volume_write & 7;
    if (!set) mask = 1u << first_channel(mask);
    int16_t target = euacx_volume_snap(*value, p->info.volume_min, p->info.volume_max, p->info.volume_res);
    for (unsigned ch = 0; ch < 3; ++ch) if (mask & (1u << ch)) {
        uint8_t data[2] = {target, (uint16_t)target >> 8};
        uint16_t entity = (f->id << 8) | p->dev->caps.ac_interface;
        esp_err_t e = set ? euacx_control(p, 0x21, 1, 0x0200 | ch, entity, data, 2) : ESP_OK;
        if (e == ESP_OK) e = euacx_control(p, 0xa1, 1, 0x0200 | ch, entity, data, 2);
        if (e != ESP_OK) return e;
        int16_t actual = (int16_t)(data[0] | (unsigned)data[1] << 8);
        if (set && actual != target) return ESP_ERR_INVALID_RESPONSE;
        *value = actual;
    }
    return ESP_OK;
}
esp_err_t euacx_hw_mute(euacx_port_t *p, bool set, bool *value)
{
    if (!p->info.has_mute || !p->dev->feature) return ESP_ERR_NOT_SUPPORTED;
    const euacx_feature_t *f = p->dev->feature;
    uint32_t mask = f->mute_read & f->mute_write & 7;
    if (!set) mask = 1u << first_channel(mask);
    bool target = *value;
    for (unsigned ch = 0; ch < 3; ++ch) if (mask & (1u << ch)) {
        uint8_t data = target;
        uint16_t entity = (f->id << 8) | p->dev->caps.ac_interface;
        esp_err_t e = set ? euacx_control(p, 0x21, 1, 0x0100 | ch, entity, &data, 1) : ESP_OK;
        if (e == ESP_OK) e = euacx_control(p, 0xa1, 1, 0x0100 | ch, entity, &data, 1);
        if (e != ESP_OK) return e;
        if (set && (bool)data != target) return ESP_ERR_INVALID_RESPONSE;
        *value = data != 0;
    }
    return ESP_OK;
}
