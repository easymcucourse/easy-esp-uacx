/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#pragma once
#include "euacx_model.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "sdkconfig.h"
#ifndef CONFIG_EUACX_USE_VERIFIED_CAPS
#define CONFIG_EUACX_USE_VERIFIED_CAPS 0
#endif

typedef struct euacx_context euacx_context_t;
typedef struct euacx_stream_state euacx_stream_state_t;
typedef struct euacx_dev euacx_dev_t;
typedef enum { REQ_OPEN, REQ_CLOSE, REQ_VOLUME_SET, REQ_VOLUME_GET,
               REQ_MUTE_SET, REQ_MUTE_GET, REQ_DEINIT } euacx_req_id_t;
typedef struct euacx_request {
    euacx_req_id_t id;
    euacx_port_t *port;
    euacx_stream_config_t cfg;
    TaskHandle_t caller;
    void *out;
    int16_t volume;
    bool mute;
    esp_err_t result;
    SemaphoreHandle_t done;
    struct euacx_request *next;
} euacx_request_t;
typedef enum { NOTICE_CONNECTED, NOTICE_STOPPED, NOTICE_DISCONNECTED } euacx_notice_id_t;
typedef struct euacx_notice {
    euacx_notice_id_t id;
    euacx_port_t *port;
    uint32_t conn;
    euacx_stop_reason_t reason;
    esp_err_t error;
    struct euacx_notice *next;
} euacx_notice_t;

struct euacx_dev {
    usb_device_handle_t usb;
    euacx_dev_caps_t caps;
    const euacx_driver_t *driver;
    void *driver_ctx;
    const euacx_feature_t *feature;
    euacx_notice_t *disconnected;
    int claimed;
    bool detached;
    int16_t volume_cache, volume_raw_min;
    bool volume_cached, mute_cache, mute_cached;
};
struct euacx_port {
    euacx_context_t *ctx;
    euacx_state_t state;
    euacx_info_t info;
    uint32_t delivered_conn;
    _Atomic bool gone;
    euacx_dev_t *dev;
    euacx_stream_state_t *stream;
    SemaphoreHandle_t space;
};
struct euacx_stream_state {
    euacx_port_t *port;
    euacx_stream_config_t cfg;
    euacx_alt_t alt;
    euacx_ring_t ring;
    SemaphoreHandle_t writer;
    TaskHandle_t owner, feed;
    _Atomic bool stop, abort, eof, feed_exited, fault, started, ready, warming;
    _Atomic uint32_t refs, inflight, feedback_q16, errors, underruns;
    uint64_t phase;
    uint8_t partial[8], partial_size;
    usb_transfer_t *transfers[CONFIG_EUACX_NUM_TRANSFERS], *feedback;
    unsigned packets, consecutive_errors;
    bool closing, restore_mute, restore_mute_valid;
    euacx_stop_reason_t reason;
    esp_err_t error;
    euacx_request_t *close_requests;
    euacx_notice_t *stopped;
};
struct euacx_context {
    euacx_config_t config;
    usb_host_client_handle_t client;
    euacx_port_t port;
    TaskHandle_t mgr, pump, cb, lib;
    QueueHandle_t req_q, int_q, cb_q;
    SemaphoreHandle_t cb_wake, task_done;
    portMUX_TYPE lock;
    euacx_notice_t *overflow_head, *overflow_tail;
    _Atomic bool addresses[128], shutting, pump_exit, cb_exit, lib_exit;
    _Atomic bool active;
    _Atomic bool pump_exited, cb_exited, lib_exited;
    _Atomic unsigned api_users;
    _Atomic unsigned ctrl_pending;
    bool installed;
    bool deferred_free;
    euacx_request_t *deinit;
};

esp_err_t euacx_control(euacx_port_t *p, uint8_t type, uint8_t request,
                        uint16_t value, uint16_t index, void *data, uint16_t size);
/* Transport seams are replaced only by the host test executable. */
esp_err_t euacx_control_transfer(euacx_port_t *p, uint8_t type, uint8_t request,
                                 uint16_t value, uint16_t index, void *data, uint16_t size);
void euacx_control_delay(uint32_t microseconds);
esp_err_t euacx_claim_interface(euacx_port_t *p, uint8_t interface, uint8_t alt);
void euacx_release_interface(euacx_port_t *p);
esp_err_t euacx_prepare_stream(euacx_port_t *p, const euacx_stream_config_t *cfg, euacx_alt_t *selected);
esp_err_t euacx_set_interface(euacx_port_t *p, uint8_t interface, uint8_t alt);
esp_err_t euacx_probe_controls(euacx_port_t *p);
esp_err_t euacx_hw_volume(euacx_port_t *p, bool set, int16_t *value);
esp_err_t euacx_hw_mute(euacx_port_t *p, bool set, bool *value);
esp_err_t euacx_clock_rate(euacx_port_t *p, const euacx_alt_t *alt, uint32_t hz);
void euacx_probe_rates(euacx_port_t *p);
void euacx_notice_send(euacx_context_t *c, euacx_notice_t *notice);
void euacx_callback_task(void *arg);
void euacx_request_reply(euacx_request_t *request, esp_err_t result);
esp_err_t euacx_request_call(euacx_context_t *c, euacx_request_t *request);
void euacx_manager_task(void *arg);
void euacx_pump_task(void *arg);
void euacx_library_task(void *arg);
void euacx_usb_event(const usb_host_client_event_msg_t *event, void *arg);
esp_err_t euacx_stream_start(euacx_port_t *p, euacx_request_t *request);
void euacx_stream_stop(euacx_port_t *p, euacx_stop_reason_t reason, esp_err_t error);
bool euacx_stream_finish(euacx_port_t *p);
void euacx_iso_start(euacx_stream_state_t *s);
void euacx_iso_dispose(euacx_stream_state_t *s);
void euacx_feed_task(void *arg);
esp_err_t euacx_stream_write(euacx_stream_state_t *s, const void *data, size_t len,
                             size_t *written, TickType_t wait);
BaseType_t euacx_task_create(euacx_context_t *c, TaskFunction_t fn, const char *name,
                             uint32_t stack, void *arg, unsigned priority, TaskHandle_t *task);
void euacx_context_destroy(euacx_context_t *c);
