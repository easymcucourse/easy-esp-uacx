/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PCM playback API. DSD/DoP types are reserved for version 2.0. */
#define EUACX_MAX_RATES 16

typedef struct euacx_port euacx_port_t;              /* one per USB root port, valid until euacx_deinit */

typedef enum {
    EUACX_STATE_DISCONNECTED,
    EUACX_STATE_ENUMERATING,
    EUACX_STATE_CONNECTED,       /* driver instance ready, no stream */
    EUACX_STATE_STREAMING,
} euacx_state_t;

typedef enum { EUACX_SPEED_FS, EUACX_SPEED_HS } euacx_speed_t;
typedef enum { EUACX_FORMAT_PCM, EUACX_FORMAT_DSD, EUACX_FORMAT_DOP } euacx_format_t;  /* what the app provides */
typedef enum { EUACX_MODE_PCM, EUACX_MODE_DOP, EUACX_MODE_NATIVE_DSD } euacx_mode_t;  /* what goes on the wire */
typedef enum { EUACX_DSD64 = 64, EUACX_DSD128 = 128, EUACX_DSD256 = 256 } euacx_dsd_rate_t;

typedef enum {
    EUACX_STOP_EOF,              /* pull mode: on_data returned EUACX_DATA_END and the buffer has drained */
    EUACX_STOP_CLOSED,           /* close, deinit, or abort of a pull stream */
    EUACX_STOP_UNPLUGGED,
    EUACX_STOP_ERROR,            /* ISO errors exceeded EUACX_MAX_ERRORS */
} euacx_stop_reason_t;

typedef struct {
    uint8_t  num_rates;
    uint32_t rates[EUACX_MAX_RATES];    /* ascending */
} euacx_rate_list_t;

typedef struct {
    uint32_t          conn_id;          /* increments on every attach of this port */
    uint16_t          vid, pid;
    char              product[32];
    const char       *driver;           /* matched driver name, "generic" if none */
    euacx_speed_t     speed;
    bool              verified;         /* capabilities come from the driver's verified table */
    euacx_rate_list_t pcm[3];           /* index 0/1/2 = 16/24/32-bit input */
    euacx_rate_list_t dsd;              /* DSD64/128/256 as 64/128/256; accepted for both DSD and DoP input */
    euacx_mode_t      dsd_mode[3];      /* per DSD rate: NATIVE_DSD or DOP */
    bool              has_volume, has_mute;
    int16_t           volume_min, volume_max, volume_res;  /* 1/256 dB */
} euacx_info_t;

/* All run in the driver's callback task, one at a time, in order. Any euacx_ API may be called from them. */
typedef struct {
    void (*on_connected)(euacx_port_t *port, const euacx_info_t *info, void *user);   /* info valid during the call */
    void (*on_disconnected)(euacx_port_t *port, uint32_t conn_id, void *user);
    void (*on_stream_stopped)(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t err, void *user);
} euacx_callbacks_t;

typedef struct {
    euacx_callbacks_t cb;               /* any member may be NULL */
    void             *user;             /* passed to every callback above */
    bool              install_usb_host; /* true: driver installs the USB Host library and runs its task */
    int               task_priority;    /* euacx_mgr and callback task; pump / lib / feed priorities are Kconfig */
    int               task_core;        /* all driver tasks, -1 = no affinity */
} euacx_config_t;

#define EUACX_CONFIG_DEFAULT() { .cb = { NULL, NULL, NULL }, .user = NULL, .install_usb_host = true, \
                                 .task_priority = 5, .task_core = -1 }

/* Pull mode: fill up to len bytes in the stream's input format (no frame alignment needed).
 * Return bytes filled, 0 = nothing available yet (driver retries, buffer may underrun), or EUACX_DATA_END. */
#define EUACX_DATA_END (-1)
typedef int (*euacx_data_cb_t)(euacx_port_t *port, void *buf, size_t len, void *user);

typedef struct {
    euacx_format_t   format;
    uint32_t         sample_rate;       /* PCM only; DoP carrier rate is derived from dsd_rate */
    uint8_t          bits;              /* PCM: 16 / 24 / 32; DoP: 24 (packed) or 32 (container) */
    uint8_t          channels;          /* 2 */
    euacx_dsd_rate_t dsd_rate;          /* DSD and DoP */
    euacx_data_cb_t  on_data;           /* NULL = push mode (euacx_write), otherwise pull mode */
    void            *data_user;         /* passed to on_data */
} euacx_stream_config_t;

esp_err_t     euacx_init(const euacx_config_t *cfg);
esp_err_t     euacx_deinit(void);

/* DAC state and capabilities */
int           euacx_port_count(void);
euacx_port_t *euacx_get_port(int index);                            /* 0 = first enabled port, NULL if out of range */
euacx_state_t euacx_get_state(euacx_port_t *port);
esp_err_t     euacx_get_info(euacx_port_t *port, euacx_info_t *out);  /* copied out */

/* playback */
esp_err_t euacx_stream_open(euacx_port_t *port, const euacx_stream_config_t *cfg, euacx_mode_t *mode);  /* mode may be NULL */
esp_err_t euacx_write(euacx_port_t *port, const void *data, size_t len, size_t *written, uint32_t timeout_ms);  /* push only */
esp_err_t euacx_stream_close(euacx_port_t *port);
/* Nonblocking: push wakes write and keeps STREAMING until the owner closes;
 * pull requests closure. May be called by any application task. */
esp_err_t euacx_stream_abort(euacx_port_t *port);

/* volume */
esp_err_t euacx_set_volume(euacx_port_t *port, int16_t db256);     /* clamped and snapped to the device step */
esp_err_t euacx_get_volume(euacx_port_t *port, int16_t *db256);
esp_err_t euacx_set_mute(euacx_port_t *port, bool mute);
esp_err_t euacx_get_mute(euacx_port_t *port, bool *mute);

#ifdef __cplusplus
}
#endif
