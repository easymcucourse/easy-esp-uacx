/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include "euacx_runtime.h"
#include "unity.h"
#include "unity_test_runner.h"
#include <stdlib.h>

typedef struct { unsigned count; bool out_of_order; } notice_result_t;
typedef struct {
    euacx_context_t context;
    SemaphoreHandle_t done;
    _Atomic unsigned failures, next_id, callers_done;
} bus_fixture_t;
static void bus_responder(void *arg)
{
    bus_fixture_t *f = arg;
    for (unsigned i = 0; i < 128; ++i) {
        euacx_request_t *r = NULL;
        xQueueReceive(f->context.req_q, &r, portMAX_DELAY);
        vTaskDelay(1); /* Make the single-entry queue contend. */
        *(int16_t *)r->out = r->volume;
        euacx_request_reply(r, ESP_OK);
    }
    while (atomic_load(&f->callers_done) != 8) vTaskDelay(1);
    SemaphoreHandle_t done = f->done;
    xSemaphoreGive(done); vTaskDelete(NULL);
}
static void bus_caller(void *arg)
{
    bus_fixture_t *f = arg;
    unsigned id = atomic_fetch_add(&f->next_id, 1);
    for (unsigned i = 0; i < 16; ++i) {
        int16_t out = -1;
        int16_t expected = id * 100 + i;
        euacx_request_t r = {.id = REQ_VOLUME_GET, .volume = expected, .out = &out};
        if (euacx_request_call(&f->context, &r) != ESP_OK || out != expected)
            atomic_fetch_add(&f->failures, 1);
    }
    SemaphoreHandle_t done = f->done;
    atomic_fetch_add(&f->callers_done, 1);
    xSemaphoreGive(done); vTaskDelete(NULL);
}
TEST_CASE("runtime bus: contended requests retain caller stack until matching replies", "[euacx][bus]")
{
    bus_fixture_t *f = calloc(1, sizeof(*f));
    TEST_ASSERT_NOT_NULL(f);
    f->context.config = (euacx_config_t)EUACX_CONFIG_DEFAULT();
    f->context.req_q = xQueueCreate(1, sizeof(euacx_request_t *));
    f->done = xSemaphoreCreateCounting(9, 0);
    TEST_ASSERT_NOT_NULL(f->context.req_q); TEST_ASSERT_NOT_NULL(f->done);
    TEST_ASSERT_EQUAL(pdPASS, euacx_task_create(&f->context, bus_responder, "bus_reply", 3072, f, 5, &f->context.mgr));
    for (unsigned i = 0; i < 8; ++i) {
        TaskHandle_t task;
        TEST_ASSERT_EQUAL(pdPASS, euacx_task_create(&f->context, bus_caller, "bus_caller", 3072, f, 5, &task));
    }
    for (unsigned i = 0; i < 9; ++i)
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(f->done, pdMS_TO_TICKS(5000)));
    TEST_ASSERT_EQUAL(0, atomic_load(&f->failures));
    vQueueDelete(f->context.req_q); vSemaphoreDelete(f->done); free(f);
    vTaskDelay(pdMS_TO_TICKS(20));
}
static void notice_stopped(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t error, void *user)
{
    (void)port; (void)reason;
    notice_result_t *result = user;
    if (error != (esp_err_t)result->count) result->out_of_order = true;
    ++result->count;
}
TEST_CASE("runtime notices: full callback queue preserves every terminal event in order", "[euacx][callback]")
{
    euacx_context_t *c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
    c->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    c->cb_q = xQueueCreate(2, sizeof(euacx_notice_t *));
    c->cb_wake = xSemaphoreCreateBinary(); c->task_done = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(c->cb_q); TEST_ASSERT_NOT_NULL(c->cb_wake); TEST_ASSERT_NOT_NULL(c->task_done);
    notice_result_t result = {0};
    c->config = (euacx_config_t)EUACX_CONFIG_DEFAULT();
    c->config.user = &result; c->config.cb.on_stream_stopped = notice_stopped;
    for (unsigned i = 0; i < 20; ++i) {
        euacx_notice_t *n = calloc(1, sizeof(*n));
        TEST_ASSERT_NOT_NULL(n);
        n->id = NOTICE_STOPPED; n->port = &c->port; n->error = i;
        euacx_notice_send(c, n);
    }
    TEST_ASSERT_NOT_NULL(c->overflow_head);
    atomic_store(&c->active, true); atomic_store(&c->cb_exit, true);
    TEST_ASSERT_EQUAL(pdPASS, euacx_task_create(c, euacx_callback_task, "notice_test", CONFIG_EUACX_CB_STACK,
                                              c, 5, &c->cb));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(c->task_done, pdMS_TO_TICKS(2000)));
    while (!atomic_load(&c->cb_exited)) vTaskDelay(1);
    TEST_ASSERT_EQUAL(20, result.count); TEST_ASSERT_FALSE(result.out_of_order);
    TEST_ASSERT_NULL(c->overflow_head);
    vQueueDelete(c->cb_q); vSemaphoreDelete(c->cb_wake); vSemaphoreDelete(c->task_done); free(c);
    vTaskDelay(pdMS_TO_TICKS(20));
}

TEST_CASE("runtime writer: partial frames, conversion, timeout and abort preserve bytes", "[euacx][stream]")
{
    euacx_port_t port = {0};
    uint8_t storage[8], out[6];
    euacx_stream_state_t s = {.port = &port, .cfg = {.bits = 16}, .alt = {.subslot = 3},
        .ring = {.data = storage, .size = sizeof(storage)}};
    port.space = xSemaphoreCreateBinary(); s.writer = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(port.space); TEST_ASSERT_NOT_NULL(s.writer);
    const uint8_t input[] = {0,0x80,0xff,0x7f,0x12,0x34,0x56,0x78};
    size_t written;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_stream_write(&s, input, 1, &written, 0));
    TEST_ASSERT_EQUAL(1, written); TEST_ASSERT_EQUAL(0, euacx_ring_used(&s.ring));
    TEST_ASSERT_EQUAL(ESP_OK, euacx_stream_write(&s, input + 1, 3, &written, 0));
    TEST_ASSERT_EQUAL(3, written); TEST_ASSERT_EQUAL(6, euacx_ring_used(&s.ring));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, euacx_stream_write(&s, input + 4, 4, &written, 0));
    TEST_ASSERT_EQUAL(0, written);
    TEST_ASSERT_EQUAL(6, euacx_ring_take(&s.ring, out, 6));
    const uint8_t expected[] = {0,0,0x80,0,0xff,0x7f};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 6);
    TEST_ASSERT_EQUAL(ESP_OK, euacx_stream_write(&s, input + 4, 4, &written, 0));
    atomic_store(&s.abort, true);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, euacx_stream_write(&s, input, 4, &written, 0));
    TEST_ASSERT_EQUAL(0, written); TEST_ASSERT_EQUAL(6, euacx_ring_used(&s.ring));
    vSemaphoreDelete(s.writer); vSemaphoreDelete(port.space);
}
TEST_CASE("runtime writer: batch conversion and PCM32 direct copy wrap correctly", "[euacx][stream]")
{
    euacx_port_t port = {0};
    uint8_t storage[32], out[24];
    euacx_stream_state_t s = {.port = &port, .cfg = {.bits = 16}, .alt = {.subslot = 3},
        .ring = {.data = storage, .size = sizeof(storage)}};
    port.space = xSemaphoreCreateBinary(); s.writer = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(port.space); TEST_ASSERT_NOT_NULL(s.writer);
    const uint8_t input[] = {0,0x80,0xff,0x7f,0,0,0xff,0xff,1,0,0xfe,0xff,0x34,0x12,0xcc,0xed};
    const uint8_t expected[] = {0,0,0x80,0,0xff,0x7f,0,0,0,0,0xff,0xff,
        0,1,0,0,0xfe,0xff,0,0x34,0x12,0,0xcc,0xed};
    size_t written;
    TEST_ASSERT_EQUAL(ESP_OK, euacx_stream_write(&s, input, 14, &written, 0));
    TEST_ASSERT_EQUAL(14, written); TEST_ASSERT_EQUAL(18, euacx_ring_used(&s.ring));
    TEST_ASSERT_EQUAL(ESP_OK, euacx_stream_write(&s, input + 14, 2, &written, 0));
    TEST_ASSERT_EQUAL(24, euacx_ring_take(&s.ring, out, 24));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 24);
    s.cfg.bits = 32; s.alt.subslot = 4;
    const uint8_t pcm32[] = {0,0,0,0x80,0xff,0xff,0xff,0x7f,0,0,0,0x80,0xff,0xff,0xff,0x7f};
    TEST_ASSERT_EQUAL(ESP_OK, euacx_stream_write(&s, pcm32, sizeof(pcm32), &written, 0));
    TEST_ASSERT_EQUAL(sizeof(pcm32), written);
    TEST_ASSERT_EQUAL(sizeof(pcm32), euacx_ring_take(&s.ring, out, sizeof(pcm32)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pcm32, out, sizeof(pcm32));
    vSemaphoreDelete(s.writer); vSemaphoreDelete(port.space);
}
