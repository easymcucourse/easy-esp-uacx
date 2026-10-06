/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
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
esp_err_t euacx_control_transfer(euacx_port_t *p, uint8_t type, uint8_t req,
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
/* Long waits yield to the USB pump; round upward to a scheduler tick. */
void euacx_control_delay(uint32_t us)
{
    if (us < 1000) esp_rom_delay_us(us);
    else vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000) + 1);
}
esp_err_t euacx_claim_interface(euacx_port_t *p, uint8_t interface, uint8_t alt)
{
    esp_err_t e = usb_host_interface_claim(p->ctx->client, p->dev->usb, interface, alt);
    if (e == ESP_OK) p->dev->claimed = interface;
    return e;
}
void euacx_release_interface(euacx_port_t *p)
{
    if (p->dev->claimed < 0) return;
    usb_host_interface_release(p->ctx->client, p->dev->usb, p->dev->claimed);
    p->dev->claimed = -1;
}
