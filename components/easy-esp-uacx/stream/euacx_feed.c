/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include "euacx_runtime.h"

void euacx_feed_task(void *arg)
{
    euacx_stream_state_t *s = arg;
    uint8_t *data = malloc(4096);
    if (!data) atomic_store(&s->fault, true);
    while (data && !atomic_load(&s->stop)) {
        if (!atomic_load(&s->ready)) { vTaskDelay(1); continue; }
        if (euacx_ring_space(&s->ring) < 2u * s->alt.subslot) {
            xSemaphoreTake(s->port->space, pdMS_TO_TICKS(CONFIG_EUACX_FEED_RETRY_MS) + 1);
            continue;
        }
        int n = s->cfg.on_data(s->port, data, 4096, s->cfg.data_user);
        if (atomic_load(&s->stop)) break;
        if (n == EUACX_DATA_END) {
            if (s->partial_size) {
                uint8_t zero[8] = {0};
                euacx_stream_write(s, zero, s->cfg.bits / 8 * 2 - s->partial_size, NULL, portMAX_DELAY);
            }
            atomic_store(&s->eof, true); break;
        }
        if (n < 0 || n > 4096) { atomic_store(&s->fault, true); break; }
        if (!n) { vTaskDelay(pdMS_TO_TICKS(CONFIG_EUACX_FEED_RETRY_MS) + 1); continue; }
        if (euacx_stream_write(s, data, n, NULL, portMAX_DELAY) != ESP_OK) break;
    }
    free(data);
    euacx_context_t *ctx = s->port->ctx;
    xTaskNotifyGive(ctx->mgr);
    /* Final access to stream and context; shutdown can free both afterwards. */
    atomic_store_explicit(&s->feed_exited, true, memory_order_release);
    vTaskDelete(NULL);
}
