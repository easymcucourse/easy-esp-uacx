/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
/**
 * @file easy_uacx.h
 * @brief Public USB Audio Class 2.0 host playback API.
 *
 * The current runtime supports one DAC on the default USB Host root port and
 * stereo PCM playback. DSD/DoP declarations are reserved for version 2.0;
 * euacx_stream_open() currently rejects both formats.
 *
 * PCM input is interleaved left/right signed, little-endian audio. Samples use
 * two bytes for 16-bit, three packed bytes for 24-bit, and four bytes for 32-bit.
 * The driver may expand samples to a wider USB container with zero-filled low
 * bytes, but does not reduce bit depth or resample audio.
 *
 * Use these APIs from task context, not from an ISR. Status callbacks may call
 * public APIs subject to ownership rules. Pull data callbacks have additional
 * restrictions documented in euacx_data_cb_t. Manager requests wait for their
 * reply after queue admission; the configured request timeout limits admission
 * only, not the total duration of an API call.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Maximum number of entries in a public rate list.
 */
#define EUACX_MAX_RATES 16

/**
 * @brief Opaque root-port handle, obtained with euacx_get_port().
 *
 * The handle stays the same across DAC unplug/reconnect events and is valid
 * only until euacx_deinit(). Do not free it or reuse it after deinitialization,
 * even if the driver is subsequently initialized again.
 */
typedef struct euacx_port euacx_port_t;

/**
 * @brief Lifecycle state of a port and its attached playback device.
 */
typedef enum {
    EUACX_STATE_DISCONNECTED, /**< No usable DAC is attached. */
    EUACX_STATE_ENUMERATING,  /**< Inspecting descriptors and capabilities. */
    EUACX_STATE_CONNECTED,    /**< DAC ready; no stream is open. */
    EUACX_STATE_STREAMING,    /**< Stream open, including startup/pending shutdown. */
} euacx_state_t;

/**
 * @brief Enumerated device speed, not the identity of the root controller.
 * EUACX_SPEED_FS is full speed (12 Mbit/s); EUACX_SPEED_HS is high speed (480 Mbit/s).
 */
typedef enum { EUACX_SPEED_FS, EUACX_SPEED_HS } euacx_speed_t;
/**
 * @brief Application input representation.
 * EUACX_FORMAT_PCM is interleaved PCM. EUACX_FORMAT_DSD and EUACX_FORMAT_DOP
 * reserve raw DSD and DSD-over-PCM input for version 2.0.
 */
typedef enum { EUACX_FORMAT_PCM, EUACX_FORMAT_DSD, EUACX_FORMAT_DOP } euacx_format_t;
/**
 * @brief USB audio transport selected by the driver.
 * EUACX_MODE_PCM is currently the only playback mode. EUACX_MODE_DOP and
 * EUACX_MODE_NATIVE_DSD reserve DoP and native DSD transport for version 2.0.
 */
typedef enum { EUACX_MODE_PCM, EUACX_MODE_DOP, EUACX_MODE_NATIVE_DSD } euacx_mode_t;
/**
 * @brief Reserved DSD multipliers relative to 44.1 kHz.
 * DSD64/128/256 correspond to 2.8224/5.6448/11.2896 Mbit/s per channel.
 */
typedef enum { EUACX_DSD64 = 64, EUACX_DSD128 = 128, EUACX_DSD256 = 256 } euacx_dsd_rate_t;

/**
 * @brief Reason reported after a successfully opened stream stops.
 */
typedef enum {
    EUACX_STOP_EOF,       /**< Pull input ended; buffered audio and USB transfers drained. */
    EUACX_STOP_CLOSED,    /**< Explicit close, deinit, or pull-stream abort. */
    EUACX_STOP_UNPLUGGED, /**< DAC was disconnected. */
    EUACX_STOP_ERROR,     /**< Stream fault, including repeated ISO errors or invalid callback output. */
} euacx_stop_reason_t;

/**
 * @brief Bounded, strictly ascending list of supported rates.
 */
typedef struct {
    uint8_t  num_rates;             /**< Valid entry count; zero means unsupported. */
    uint32_t rates[EUACX_MAX_RATES]; /**< Read only indices below num_rates. PCM values are in Hz. */
} euacx_rate_list_t;

/**
 * @brief Snapshot of DAC identity and usable playback capabilities.
 *
 * PCM lists describe application input formats, which may use wider containers
 * on the device. Verified lists are filtered by the tested capability table and
 * available descriptors/bandwidth. Otherwise, capabilities come from the device.
 * If rate discovery fails, candidate rates are validated when opening a stream.
 *
 * Volume range fields are meaningful only when has_volume is true. DSD fields
 * are reserved; the current runtime advertises an empty dsd list. Use conn_id
 * to associate delayed notifications with the correct connection.
 */
typedef struct {
    uint32_t          conn_id;          /**< Connection generation incremented on each accepted attach. */
    uint16_t          vid, pid;         /**< USB vendor ID and product ID, respectively. */
    char              product[32];     /**< NUL-terminated product name; non-ASCII characters become '?'. */
    const char       *driver;           /**< Borrowed name with static lifetime; fallback is "generic". */
    uint32_t          driver_flags;     /**< Diagnostic behavior flags defined in the private driver header. */
    euacx_speed_t     speed;            /**< Enumerated USB device speed. */
    bool              verified;        /**< True when a verified capability table filters this snapshot. */
    euacx_rate_list_t pcm[3];           /**< Hz lists for 16/24/32-bit input at indices 0/1/2. */
    euacx_rate_list_t dsd;              /**< Reserved DSD multipliers 64/128/256, not rates in Hz. */
    euacx_mode_t      dsd_mode[3];      /**< Reserved transport modes for DSD64/128/256, respectively. */
    bool              has_volume, has_mute; /**< Usable hardware volume and mute, respectively. */
    int16_t           volume_min, volume_max, volume_res;  /**< Minimum, maximum, and positive step in 1/256 dB. */
} euacx_info_t;


/**
 * @brief Optional status notifications, serialized in a dedicated callback task.
 *
 * Notifications run one at a time in order. Public APIs may be called, subject
 * to stream ownership rules. Slow callbacks delay later notifications while
 * manager/USB tasks continue running. Keep user context alive until callbacks
 * finish, including any final notifications during deinit.
 */
typedef struct {
    /**
     * @brief Called after successful enumeration makes a DAC usable.
     * @param[in] port Port on which the DAC was attached.
     * @param[in] info Snapshot valid only during this call; copy it if needed later.
     * @param[in] user Context from euacx_config_t.user; may be NULL.
     */
    void (*on_connected)(euacx_port_t *port, const euacx_info_t *info, void *user);
    /**
     * @brief Called when a previously announced connection is released.
     * @param[in] port Port whose DAC was removed or released during deinit.
     * @param[in] conn_id Generation of the connection being released.
     * @param[in] user Context from euacx_config_t.user; may be NULL.
     */
    void (*on_disconnected)(euacx_port_t *port, uint32_t conn_id, void *user);
    /**
     * @brief Called once after a successfully opened stream has been released.
     * @param[in] port Port whose stream stopped.
     * @param[in] reason EOF, explicit closure, disconnection, or stream fault.
     * @param[in] err Stream error for EUACX_STOP_ERROR; ESP_OK for other reasons.
     * @param[in] user Context from euacx_config_t.user; may be NULL.
     */
    void (*on_stream_stopped)(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t err, void *user);
} euacx_callbacks_t;

/**
 * @brief Driver settings copied by euacx_init(); pointed-to user data is borrowed.
 */
typedef struct {
    euacx_callbacks_t cb;               /**< Each callback function pointer may be NULL. */
    void             *user;             /**< Borrowed status-callback context; may be NULL. */
    /**
     * true: install and manage USB Host. false: the application must install it
     * first, service library events, manage root-port recovery, and uninstall it.
     */
    bool              install_usb_host;
    /**
     * Manager/callback priority: >= 1 and below CONFIG_EUACX_PUMP_PRIORITY.
     * Pump/library/feed priorities use Kconfig; feed must be below pump.
     */
    int               task_priority;
    int               task_core;        /**< CPU affinity for all driver tasks: -1 for none, otherwise a valid CPU index. */
} euacx_config_t;

/**
 * @brief Initializer: no callbacks, owned USB Host, priority 5, no CPU affinity.
 */
#define EUACX_CONFIG_DEFAULT() { .cb = { NULL, NULL, NULL }, .user = NULL, .install_usb_host = true, \
                                 .task_priority = 5, .task_core = -1 }

/**
 * @brief Pull callback result indicating that no more input will arrive.
 */
#define EUACX_DATA_END (-1)
/**
 * @brief Supply audio bytes from the dedicated pull-mode feed task.
 *
 * Fill up to len bytes in the stream input format. Neither len nor the supplied
 * byte count needs to align to a stereo frame. Return final data as a positive
 * count, then EUACX_DATA_END on a later call. At EOF an incomplete frame is
 * padded with zero bytes; buffered audio and in-flight USB transfers drain
 * before EUACX_STOP_EOF is reported.
 *
 * Return promptly. Returning 0 causes a retry; insufficient buffered data
 * during playback produces silence and counts as an underrun. This callback
 * may query state/capabilities and adjust volume/mute, but must not call
 * euacx_write(), open/close the same port, or call euacx_deinit(). A close or
 * deinit requested elsewhere waits for the executing callback to return.
 *
 * @param[in] port Port with the active pull stream.
 * @param[out] buf Driver-owned destination, valid only during this invocation.
 * @param[in] len Maximum number of input bytes that may be written to buf.
 * @param[in] user Borrowed context from euacx_stream_config_t.data_user.
 * @return 1..len for supplied bytes, 0 for temporarily unavailable data, or
 *         EUACX_DATA_END for EOF. Other negative values or values above len
 *         cause EUACX_STOP_ERROR.
 */
typedef int (*euacx_data_cb_t)(euacx_port_t *port, void *buf, size_t len, void *user);

/**
 * @brief Settings copied when a stream is opened.
 *
 * Zero-initialize before assigning fields. Current playback accepts only PCM,
 * a nonzero supported sample rate, 16/24/32-bit input, and two channels.
 * on_data selects push versus pull mode; data_user is borrowed, not copied.
 */
typedef struct {
    euacx_format_t   format;           /**< Application input format; currently PCM only. */
    /**
     * PCM frames per second (Hz); must appear in the matching pcm list.
     * Reserved DoP transport would derive its carrier rate from dsd_rate.
     */
    uint32_t         sample_rate;
    uint8_t          bits;             /**< PCM bits per sample: 16, packed 24, or 32. Reserved DoP: 24/32. */
    uint8_t          channels;         /**< Must be 2, with samples interleaved left then right. */
    euacx_dsd_rate_t dsd_rate;          /**< Reserved for DSD/DoP; ignored for PCM. */
    euacx_data_cb_t  on_data;           /**< NULL for push via euacx_write(); non-NULL for pull. */
    void            *data_user;         /**< Borrowed on_data context; keep valid until the stream has stopped. */
} euacx_stream_config_t;

/**
 * @brief Initialize the driver and begin monitoring for a DAC.
 *
 * Does not wait for a DAC to connect. cfg is copied and may be reused after
 * return; its user context must remain valid while callbacks can run.
 *
 * @param[in] cfg Non-NULL configuration; start with EUACX_CONFIG_DEFAULT().
 * @retval ESP_OK Driver tasks and USB client are ready.
 * @retval ESP_ERR_INVALID_ARG NULL cfg, invalid priority/core, or incompatible
 *                             configured feed/pump priorities.
 * @retval ESP_ERR_INVALID_STATE Driver already initialized or initializing.
 * @retval ESP_ERR_NO_MEM Driver resources or tasks could not be allocated.
 * @return USB Host installation/registration errors may also be propagated.
 */
esp_err_t     euacx_init(const euacx_config_t *cfg);
/**
 * @brief Stop playback and release devices, driver tasks, and resources.
 *
 * Only a USB Host library installed by this driver is uninstalled. Waits for
 * pending API users, USB transfers, and executing pull callbacks; it does not
 * forcibly terminate callback code. Do not use port handles after success.
 *
 * May be called from a status callback: final memory release is then deferred
 * until that callback returns and queued notifications are delivered. Must
 * not be called from the pull data callback.
 *
 * @retval ESP_OK Shutdown complete, or final release deferred for the calling
 *                status callback.
 * @retval ESP_ERR_INVALID_STATE Driver inactive, shutdown already in progress,
 *                               or caller is a forbidden task.
 * @retval ESP_ERR_TIMEOUT Shutdown request could not be queued in time.
 */
esp_err_t     euacx_deinit(void);

/* DAC state and capabilities */
/**
 * @brief Get the number of root-port handles exposed by the driver.
 * @return 1 while the current driver is active, otherwise 0. This is not the
 *         number of connected DACs.
 */
int           euacx_port_count(void);
/**
 * @brief Obtain a borrowed port handle even when no DAC is attached.
 * @param[in] index Zero-based enabled-port index; currently only 0 is supported.
 * @return Handle valid until deinit, or NULL if index is invalid or driver inactive.
 */
euacx_port_t *euacx_get_port(int index);
/**
 * @brief Read current port state without submitting a manager request.
 * @param[in] port Handle obtained from euacx_get_port().
 * @return State snapshot, or EUACX_STATE_DISCONNECTED if driver/handle is
 *         unavailable or removal has been observed. A snapshot does not
 *         guarantee that a subsequent operation will succeed.
 */
euacx_state_t euacx_get_state(euacx_port_t *port);
/**
 * @brief Copy the connected DAC identity and capability snapshot.
 * @param[in] port Valid, non-NULL port handle.
 * @param[out] out Non-NULL caller-owned destination; written only on success.
 * @retval ESP_OK Snapshot copied in CONNECTED or STREAMING state.
 * @retval ESP_ERR_INVALID_ARG port or out is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/port unavailable, DAC not ready, or DAC removed.
 */
esp_err_t     euacx_get_info(euacx_port_t *port, euacx_info_t *out);

/* playback */
/**
 * @brief Open one PCM playback stream on a connected DAC.
 *
 * Prepares the clock/interface and allocates resources. Data is supplied with
 * euacx_write() in push mode or on_data in pull mode. Supported hardware mute
 * protects startup; the previous mute state is restored after the initial
 * silent USB pipeline is established.
 *
 * The caller becomes the stream owner. Only this task may write/close a push
 * stream. Other tasks may close a pull stream, except its data callback task.
 * Opening from that data callback task is also forbidden.
 *
 * @param[in] port Valid handle in EUACX_STATE_CONNECTED.
 * @param[in] cfg Non-NULL settings, copied before the call returns.
 * @param[out] mode Optional selected USB transport mode; may be NULL. Written
 *                  only on success, currently as EUACX_MODE_PCM.
 * @retval ESP_OK Stream opened and port entered STREAMING.
 * @retval ESP_ERR_INVALID_ARG NULL port/cfg, zero PCM rate, non-stereo channels,
 *                             or PCM bit depth other than 16/24/32.
 * @retval ESP_ERR_INVALID_STATE Driver/DAC unavailable, stream already open,
 *                               or caller is the pull data task.
 * @retval ESP_ERR_NOT_SUPPORTED Unsupported format/rate or no usable device alt.
 * @retval ESP_ERR_NO_MEM Stream/transfer/buffer/task allocation failed.
 * @retval ESP_ERR_TIMEOUT Request admission or a USB control transfer timed out.
 * @return Other device-control or USB Host errors may also be propagated.
 */
esp_err_t euacx_stream_open(euacx_port_t *port, const euacx_stream_config_t *cfg, euacx_mode_t *mode);
/**
 * @brief Copy PCM bytes into an active push stream internal buffer.
 *
 * Only the task that opened the push stream may write. Arbitrary byte lengths
 * are accepted; incomplete frames are retained across calls. Accepted bytes
 * are copied, so input memory may be reused after return. Buffer acceptance
 * does not mean the DAC has already played the bytes.
 *
 * A timeout or abort may follow partial acceptance. Inspect written even on
 * error and retry only the unaccepted suffix. Timeout uses FreeRTOS tick
 * resolution and limits this write's wait for buffer space.
 *
 * @param[in] port Valid handle with an active, non-aborted push stream.
 * @param[in] data Input PCM bytes; may be NULL only when len is zero.
 * @param[in] len Byte count; no stereo-frame alignment is required.
 * @param[out] written Optional accepted byte count, including partial-frame
 *                     bytes; may be NULL. Set to zero before validation.
 * @param[in] timeout_ms Wait in milliseconds: 0 does not wait for space,
 *                       UINT32_MAX waits indefinitely; positive waits use ticks.
 * @retval ESP_OK All len bytes accepted, including a valid zero-length write.
 * @retval ESP_ERR_INVALID_ARG NULL port, or NULL data with nonzero len.
 * @retval ESP_ERR_INVALID_STATE No usable push stream, wrong caller, abort,
 *                               shutdown, or device removal.
 * @retval ESP_ERR_TIMEOUT Timed out before accepting all bytes.
 */
esp_err_t euacx_write(euacx_port_t *port, const void *data, size_t len, size_t *written, uint32_t timeout_ms);
/**
 * @brief Stop playback immediately and wait for stream resources to be released.
 *
 * Discards unplayed buffered audio and incomplete input frames. This does not
 * drain audio; push mode has no EOF/drain API. Waits for USB cancellation and
 * an executing pull callback to return. A successfully opened stream produces
 * on_stream_stopped; explicit closure normally reports EUACX_STOP_CLOSED.
 * With no stream, repeated calls succeed while the driver is active.
 *
 * @param[in] port Valid handle. For an attached push stream the caller must
 *                 be its opening task; a pull data callback cannot close.
 * @retval ESP_OK Stream released, or no stream was open.
 * @retval ESP_ERR_INVALID_ARG port is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/port unavailable or caller lacks permission.
 * @retval ESP_ERR_TIMEOUT Close request could not be queued in time.
 */
esp_err_t euacx_stream_close(euacx_port_t *port);
/**
 * @brief Request an abort without waiting for stream shutdown.
 *
 * Push mode wakes a blocked writer and rejects subsequent writes. The stream
 * remains open in STREAMING until its owner calls euacx_stream_close().
 * Pull mode requests closure with EUACX_STOP_CLOSED; an executing data callback
 * must return before resources can be released. Any application task may
 * abort. With no stream this is a successful no-op while the driver is active.
 *
 * @param[in] port Valid port handle to abort.
 * @retval ESP_OK Abort requested, or no stream open; closure may still be pending.
 * @retval ESP_ERR_INVALID_ARG port is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/port unavailable or shutting down.
 */
esp_err_t euacx_stream_abort(euacx_port_t *port);

/* volume */
/**
 * @brief Set hardware playback volume on supported master/left/right controls.
 *
 * Clamps to volume_min/volume_max and rounds down to a supported step relative
 * to volume_min. For devices where the raw minimum means mute, requests at or
 * below that raw minimum become mute; advertised volume_min excludes it.
 * Readback is verified unless the driver declares it unreliable, in which
 * case successful writes update a cache. PCM sample bytes are unchanged.
 * Any application task may adjust volume while connected or streaming.
 *
 * @param[in] port Valid handle in CONNECTED or STREAMING state.
 * @param[in] db256 Signed volume in 1/256 dB; -2688 represents -10.5 dB.
 * @retval ESP_OK Setting completed and readback matched when enabled.
 * @retval ESP_ERR_INVALID_ARG port is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/DAC unavailable or shutting down.
 * @retval ESP_ERR_NOT_SUPPORTED No usable hardware volume control.
 * @retval ESP_ERR_INVALID_RESPONSE Device readback did not match.
 * @retval ESP_ERR_TIMEOUT Request admission or USB control transfer timed out.
 * @return Other USB control errors may be propagated. Multi-channel updates
 *         are not atomic: a failed call may already have changed some channels.
 */
esp_err_t euacx_set_volume(euacx_port_t *port, int16_t db256);
/**
 * @brief Read volume from the first supported master/left/right control.
 *
 * Devices with unreliable readback return the last successful cached SET
 * value. A mute action does not overwrite that cached volume value.
 *
 * @param[in] port Valid handle in CONNECTED or STREAMING state.
 * @param[out] db256 Non-NULL destination in 1/256 dB. Initialized to zero before
 *                  the request; interpret it only on ESP_OK.
 * @retval ESP_OK Volume read or retrieved from cache.
 * @retval ESP_ERR_INVALID_ARG port or db256 is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/DAC unavailable, shutting down, or cache invalid.
 * @retval ESP_ERR_NOT_SUPPORTED No usable hardware volume control.
 * @retval ESP_ERR_TIMEOUT Request admission or USB control transfer timed out.
 * @return Other USB control errors may also be propagated.
 */
esp_err_t euacx_get_volume(euacx_port_t *port, int16_t *db256);
/**
 * @brief Set hardware mute on supported master/left/right controls.
 *
 * Readback is verified unless the device uses cached controls. During startup,
 * a successful request also updates the mute state restored after the initial
 * silent pipeline. Any application task may adjust mute.
 *
 * @param[in] port Valid handle in CONNECTED or STREAMING state.
 * @param[in] mute true to mute playback, false to unmute.
 * @retval ESP_OK Setting completed and readback matched when enabled.
 * @retval ESP_ERR_INVALID_ARG port is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/DAC unavailable or shutting down.
 * @retval ESP_ERR_NOT_SUPPORTED No usable hardware mute control.
 * @retval ESP_ERR_INVALID_RESPONSE Device readback did not match.
 * @retval ESP_ERR_TIMEOUT Request admission or USB control transfer timed out.
 * @return Other USB control errors may be propagated. Multi-channel updates
 *         are not atomic: a failed call may already have changed some channels.
 */
esp_err_t euacx_set_mute(euacx_port_t *port, bool mute);
/**
 * @brief Read mute from the first supported master/left/right control.
 *
 * Devices with unreliable readback return the last successful cached mute
 * setting. Any application task may query mute.
 *
 * @param[in] port Valid handle in CONNECTED or STREAMING state.
 * @param[out] mute Non-NULL destination; true means muted. Initialized to false
 *                 before the request; interpret it only on ESP_OK.
 * @retval ESP_OK Mute read or retrieved from cache.
 * @retval ESP_ERR_INVALID_ARG port or mute is NULL.
 * @retval ESP_ERR_INVALID_STATE Driver/DAC unavailable, shutting down, or cache invalid.
 * @retval ESP_ERR_NOT_SUPPORTED No usable hardware mute control.
 * @retval ESP_ERR_TIMEOUT Request admission or USB control transfer timed out.
 * @return Other USB control errors may also be propagated.
 */
esp_err_t euacx_get_mute(euacx_port_t *port, bool *mute);

#ifdef __cplusplus
}
#endif
