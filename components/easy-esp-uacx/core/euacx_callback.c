/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include "euacx_runtime.h"

void euacx_notice_send(euacx_context_t *c, euacx_notice_t *n)
{
    /* STOPPED/DISCONNECTED nodes are reserved before open/attach succeeds.
     * Overflow never loses a terminal notification or changes FIFO ordering. */
    n->next = NULL;
    portENTER_CRITICAL(&c->lock);
    if (c->overflow_head || xQueueSend(c->cb_q, &n, 0) != pdTRUE) {
        if (c->overflow_tail) c->overflow_tail->next = n;
        else c->overflow_head = n;
        c->overflow_tail = n;
    }
    portEXIT_CRITICAL(&c->lock);
    xSemaphoreGive(c->cb_wake);
}

void euacx_callback_task(void *arg)
{
    euacx_context_t *c = arg;
    while (!atomic_load(&c->active)) vTaskDelay(1);
    for (;;) {
        euacx_notice_t *n = NULL;
        portENTER_CRITICAL(&c->lock);
        if (xQueueReceive(c->cb_q, &n, 0) != pdTRUE && c->overflow_head) {
            n = c->overflow_head;
            c->overflow_head = n->next;
            if (!c->overflow_head) c->overflow_tail = NULL;
        }
        portEXIT_CRITICAL(&c->lock);
        if (!n) {
            if (atomic_load(&c->cb_exit)) break;
            xSemaphoreTake(c->cb_wake, pdMS_TO_TICKS(20));
            continue;
        }
        euacx_port_t *p = n->port;
        if (n->id == NOTICE_CONNECTED) {
            euacx_info_t info;
            portENTER_CRITICAL(&c->lock);
            bool valid = p->info.conn_id == n->conn && !atomic_load(&p->gone) &&
                (p->state == EUACX_STATE_CONNECTED || p->state == EUACX_STATE_STREAMING);
            if (valid) { info = p->info; p->delivered_conn = n->conn; }
            portEXIT_CRITICAL(&c->lock);
            if (valid && c->config.cb.on_connected) c->config.cb.on_connected(p, &info, c->config.user);
        } else if (n->id == NOTICE_STOPPED) {
            if (c->config.cb.on_stream_stopped) c->config.cb.on_stream_stopped(p, n->reason, n->error, c->config.user);
        } else {
            portENTER_CRITICAL(&c->lock);
            bool valid = p->delivered_conn == n->conn;
            if (valid) p->delivered_conn = 0;
            portEXIT_CRITICAL(&c->lock);
            if (valid && c->config.cb.on_disconnected) c->config.cb.on_disconnected(p, n->conn, c->config.user);
        }
        free(n);
    }
    bool deferred = c->deferred_free;
    xSemaphoreGive(c->task_done);
    atomic_store(&c->cb_exited, true);
    if (deferred) euacx_context_destroy(c);
    vTaskDelete(NULL);
}
