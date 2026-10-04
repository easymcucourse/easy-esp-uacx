#include "audio_output.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio_visualizer.h"
#include "esp_intr_alloc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2s_output.h"
#include "sb_play3_output.h"
#include "usb/uac2_desc.h"
#include "usb/uac2_host.h"
#include "usb/usb_host.h"

#define CX31993_VID 0x06CB
#define CX31993_PID 0x1594

#define HOST_TASK_STACK_SIZE   4096
#define DRIVER_TASK_STACK_SIZE 6144
#define DEVICE_TASK_STACK_SIZE 8192
#define HOST_TASK_PRIORITY     21
#define DRIVER_TASK_PRIORITY   20

#define OUTPUT_BLOCK_FRAMES 256U
#define DEFAULT_SAMPLE_RATE 48000U
#define CX_READY_BIT BIT0
#define USB_PLAYBACK_CHANNELS 2U
#define USB_PLAYBACK_BITS 24U
#define USB_WRITE_TIMEOUT_MS 1000U
#define CX31993_WINDOWS_50PCT_DB256 ((int16_t)0xF580)
#define VOLUME_PERMILLE_MAX 1000U

static const char *TAG = "cx31993_output";

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_output_mutex;
static uac2_host_device_handle_t s_device;
static bool s_stream_active;
static uint8_t s_usb_bit_resolution;
static uint32_t s_sample_rate;
static uint32_t s_usb_sample_rate;
static uint32_t s_usb_source_rate;
static uint32_t s_usb_failed_source_rate;
static bool s_usb_is_cx31993;

static portMUX_TYPE s_connect_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connecting;
static uint8_t s_pending_addr;
static uint8_t s_pending_iface;
static TaskHandle_t s_device_task;
static _Atomic bool s_disconnect_seen;

static int32_t scale_sample_q31(int32_t sample, unsigned volume_permille)
{
    int64_t scaled = (int64_t)sample * (int64_t)volume_permille;
    /* Round symmetrically instead of truncating low-level samples. */
    scaled += scaled >= 0 ? (int64_t)VOLUME_PERMILLE_MAX / 2
                          : -(int64_t)VOLUME_PERMILLE_MAX / 2;
    return (int32_t)(scaled / (int64_t)VOLUME_PERMILLE_MAX);
}

static void apply_cx31993_controls(uac2_host_device_handle_t device)
{
    esp_err_t error = uac2_host_device_set_mute(device, 0, false);
    ESP_LOGI(TAG, "CX31993 mute master OFF: %s", esp_err_to_name(error));

    error = uac2_host_device_set_volume(
        device, 1, CX31993_WINDOWS_50PCT_DB256);
    ESP_LOGI(TAG, "CX31993 left volume -10.50 dB: %s",
             esp_err_to_name(error));
    error = uac2_host_device_set_volume(
        device, 2, CX31993_WINDOWS_50PCT_DB256);
    ESP_LOGI(TAG, "CX31993 right volume -10.50 dB: %s",
             esp_err_to_name(error));
}

static bool inspect_usb_audio(uac2_host_device_handle_t device,
                              bool *is_cx31993)
{
    uac2_device_info_t info;
    const esp_err_t error = uac2_host_device_get_info(device, &info);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "cannot read UAC2 device info: %s", esp_err_to_name(error));
        return false;
    }

    ESP_LOGI(TAG,
             "UAC2 device discovered: VID=0x%04X PID=0x%04X "
             "manufacturer='%s' product='%s' bcdADC=0x%04X",
             info.vid, info.pid, info.manufacturer, info.product, info.bcdADC);
    uac2_host_device_print_info(device);
    *is_cx31993 = info.vid == CX31993_VID && info.pid == CX31993_PID;
    return true;
}

/* s_output_mutex must be held. A failed USB start deliberately leaves I2S as
 * the active fallback instead of failing playback. */
static esp_err_t start_usb_stream_locked(uint32_t sample_rate)
{
    if (s_device == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_stream_active && s_usb_failed_source_rate == sample_rate) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_stream_active && s_usb_source_rate == sample_rate) {
        return ESP_OK;
    }
    if (s_stream_active) {
        (void)uac2_host_device_stop(s_device);
        s_stream_active = false;
    }

    uac2_host_stream_config_t stream_config = {
        .channels = USB_PLAYBACK_CHANNELS,
        .bit_resolution = USB_PLAYBACK_BITS,
        .sample_freq = sample_rate,
        .flags = 0,
    };
    esp_err_t error = uac2_host_device_start(s_device, &stream_config);
    if (error != ESP_OK) {
        stream_config.bit_resolution = 16;
        error = uac2_host_device_start(s_device, &stream_config);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG,
                 "USB playback unavailable at %" PRIu32
                 " Hz/24-or-16-bit stereo (%s); keeping I2S output",
                 sample_rate, esp_err_to_name(error));
        s_usb_bit_resolution = 0;
        s_usb_sample_rate = 0;
        s_usb_source_rate = 0;
        s_usb_failed_source_rate = sample_rate;
        return error;
    }

    s_stream_active = true;
    s_usb_bit_resolution = stream_config.bit_resolution;
    s_usb_sample_rate = stream_config.sample_freq;
    s_usb_source_rate = sample_rate;
    s_usb_failed_source_rate = 0;
    ESP_LOGI(TAG,
             "USB AUDIO SELECTED: %" PRIu32
             " Hz native, %u-bit stereo; no sample-rate conversion; "
             "USB has priority over I2S",
             s_usb_sample_rate, s_usb_bit_resolution);
    return ESP_OK;
}

static esp_err_t write_stereo_q31(const int32_t *pcm, size_t frame_count)
{
    static uint8_t usb_pcm[OUTPUT_BLOCK_FRAMES * USB_PLAYBACK_CHANNELS * 3];

    /* PLAY! 3 is UAC1 and has first priority when its native-rate stream is
     * available. prepare() is a fast no-op after the stream is configured. */
    if (sb_play3_output_prepare(s_sample_rate) == ESP_OK &&
        sb_play3_output_is_active()) {
        const esp_err_t play3_error =
            sb_play3_output_write_q31(pcm, frame_count);
        if (play3_error == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "PLAY! 3 output failed; trying UAC2/I2S fallback");
    }

    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    if (s_device != NULL && s_stream_active) {
        uint8_t *destination = usb_pcm;
        for (size_t sample_index = 0;
             sample_index < frame_count * USB_PLAYBACK_CHANNELS;
             ++sample_index) {
            if (s_usb_bit_resolution == 24) {
                const int32_t sample_24 = pcm[sample_index] >> 8;
                *destination++ = (uint8_t)(sample_24 & 0xff);
                *destination++ = (uint8_t)((sample_24 >> 8) & 0xff);
                *destination++ = (uint8_t)((sample_24 >> 16) & 0xff);
            } else {
                const int16_t sample_16 =
                    (int16_t)(pcm[sample_index] >> 16);
                *destination++ = (uint8_t)(sample_16 & 0xff);
                *destination++ = (uint8_t)((sample_16 >> 8) & 0xff);
            }
        }

        const uint32_t bytes_per_sample = s_usb_bit_resolution / 8U;
        const esp_err_t error = uac2_host_device_write(
            s_device, usb_pcm,
            (uint32_t)(frame_count * USB_PLAYBACK_CHANNELS * bytes_per_sample),
            USB_WRITE_TIMEOUT_MS);
        if (error == ESP_OK) {
            xSemaphoreGive(s_output_mutex);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "USB audio write failed (%s); falling back to I2S",
                 esp_err_to_name(error));
        (void)uac2_host_device_stop(s_device);
        s_usb_failed_source_rate = s_usb_source_rate;
        s_stream_active = false;
        s_usb_bit_resolution = 0;
        s_usb_sample_rate = 0;
        s_usb_source_rate = 0;
    }
    xSemaphoreGive(s_output_mutex);

    return i2s_output_write(pcm, frame_count);
}

static void device_event_callback(uac2_host_device_handle_t device,
                                  uac2_host_device_event_t event,
                                  void *argument)
{
    (void)device;
    (void)argument;
    switch (event) {
    case UAC2_HOST_DEVICE_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "CX31993 disconnected; unloading device instance");
        atomic_store(&s_disconnect_seen, true);
        if (s_device_task != NULL) {
            xTaskNotifyGive(s_device_task);
        }
        break;
    case UAC2_HOST_DEVICE_EVENT_STREAM_ERROR:
        ESP_LOGE(TAG, "CX31993 stream stopped after repeated USB errors");
        atomic_store(&s_disconnect_seen, true);
        if (s_device_task != NULL) {
            xTaskNotifyGive(s_device_task);
        }
        break;
    case UAC2_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "CX31993 isochronous transfer error");
        break;
    default:
        break;
    }
}

static void cx31993_device_task(void *argument)
{
    (void)argument;
    uint8_t address;
    uint8_t interface_number;
    portENTER_CRITICAL(&s_connect_lock);
    address = s_pending_addr;
    interface_number = s_pending_iface;
    portEXIT_CRITICAL(&s_connect_lock);

    uac2_host_device_handle_t device = NULL;
    const uac2_host_device_config_t config = {
        .addr = address,
        .iface_num = interface_number,
        .buffer_size = 96 * 2 * 3 * 24,
        .buffer_threshold = 96 * 2 * 3 * 8,
        .callback = device_event_callback,
        .callback_arg = NULL,
    };

    ESP_LOGI(TAG, "dynamically opening UAC2 TX addr=%u interface=%u",
             address, interface_number);
    esp_err_t error = uac2_host_device_open(&config, &device);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "UAC2 open failed: %s", esp_err_to_name(error));
        goto done;
    }
    bool is_cx31993 = false;
    if (!inspect_usb_audio(device, &is_cx31993)) {
        goto done;
    }

    if (is_cx31993) {
        apply_cx31993_controls(device);
    } else {
        ESP_LOGI(TAG, "generic UAC2 playback device accepted");
    }

    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    s_device = device;
    s_usb_is_cx31993 = is_cx31993;
    s_stream_active = false;
    s_usb_failed_source_rate = 0;
    if (s_sample_rate != 0) {
        (void)start_usb_stream_locked(s_sample_rate);
    }
    xEventGroupSetBits(s_events, CX_READY_BIT);
    xSemaphoreGive(s_output_mutex);
    ESP_LOGI(TAG,
             "%s DRIVER LOAD PASS; USB playback is preferred when its "
             "stream is active",
             is_cx31993 ? "CX31993" : "UAC2");

    while (!atomic_load(&s_disconnect_seen)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    xEventGroupClearBits(s_events, CX_READY_BIT);
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    s_device = NULL;
    const bool was_streaming = s_stream_active;
    s_stream_active = false;
    s_usb_bit_resolution = 0;
    s_usb_sample_rate = 0;
    s_usb_source_rate = 0;
    s_usb_failed_source_rate = 0;
    s_usb_is_cx31993 = false;
    xSemaphoreGive(s_output_mutex);

    if (was_streaming) {
        (void)uac2_host_device_stop(device);
    }
    ESP_LOGI(TAG, "USB audio unavailable; I2S playback restored");

done:
    if (device != NULL) {
        ESP_LOGI(TAG, "closing CX31993/UAC2 device instance");
        (void)uac2_host_device_close(device);
    }
    atomic_store(&s_disconnect_seen, false);
    portENTER_CRITICAL(&s_connect_lock);
    s_device_task = NULL;
    s_connecting = false;
    portEXIT_CRITICAL(&s_connect_lock);
    vTaskDelete(NULL);
}

static void driver_event_callback(uint8_t address, uint8_t interface_number,
                                  uac2_host_driver_event_t event,
                                  void *argument)
{
    (void)argument;
    if (event == UAC2_HOST_DRIVER_EVENT_RX_CONNECTED) {
        ESP_LOGI(TAG, "UAC2 RX interface addr=%u interface=%u ignored",
                 address, interface_number);
        return;
    }
    if (event != UAC2_HOST_DRIVER_EVENT_TX_CONNECTED) {
        return;
    }

    bool create_task = false;
    portENTER_CRITICAL(&s_connect_lock);
    if (!s_connecting) {
        s_connecting = true;
        s_pending_addr = address;
        s_pending_iface = interface_number;
        create_task = true;
    }
    portEXIT_CRITICAL(&s_connect_lock);
    if (!create_task) {
        return;
    }

    ESP_LOGI(TAG, "UAC2 TX detected; loading USB audio device dynamically");
    const BaseType_t created = xTaskCreatePinnedToCore(
        cx31993_device_task, "cx31993_device", DEVICE_TASK_STACK_SIZE, NULL,
        DRIVER_TASK_PRIORITY + 1, &s_device_task, 0);
    if (created != pdPASS) {
        portENTER_CRITICAL(&s_connect_lock);
        s_connecting = false;
        s_device_task = NULL;
        portEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGE(TAG, "cannot create CX31993 device task");
    }
}

static void usb_host_library_task(void *argument)
{
    TaskHandle_t caller = (TaskHandle_t)argument;
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    const esp_err_t error = usb_host_install(&config);
    xTaskNotify(caller, (uint32_t)error, eSetValueWithOverwrite);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(error));
        vTaskDelete(NULL);
    }

    for (;;) {
        uint32_t event_flags = 0;
        const esp_err_t event_error =
            usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_error != ESP_OK) {
            ESP_LOGE(TAG, "USB Host event failure: %s",
                     esp_err_to_name(event_error));
        }
        if ((event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0) {
            (void)usb_host_device_free_all();
        }
    }
}

esp_err_t audio_output_init(void)
{
    if (s_events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(i2s_output_init(), TAG, "I2S output init failed");

    s_events = xEventGroupCreate();
    s_output_mutex = xSemaphoreCreateMutex();
    if (s_events == NULL || s_output_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        usb_host_library_task, "usb_host", HOST_TASK_STACK_SIZE,
        xTaskGetCurrentTaskHandle(), HOST_TASK_PRIORITY, NULL, 0);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t install_result = ESP_FAIL;
    if (xTaskNotifyWait(0, UINT32_MAX, &install_result,
                        pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if ((esp_err_t)install_result != ESP_OK) {
        return (esp_err_t)install_result;
    }

    const uac2_host_driver_config_t driver_config = {
        .create_background_task = true,
        .task_priority = DRIVER_TASK_PRIORITY,
        .stack_size = DRIVER_TASK_STACK_SIZE,
        .core_id = 0,
        .callback = driver_event_callback,
        .callback_arg = NULL,
    };
    const esp_err_t error = uac2_host_install(&driver_config);
    if (error != ESP_OK) {
        return error;
    }

    const esp_err_t play3_error = sb_play3_output_init();
    if (play3_error != ESP_OK) {
        return play3_error;
    }

    ESP_LOGI(TAG,
             "automatic output ready: SB PLAY! 3 UAC1 / UAC2 USB on "
             "GPIO19/20 has priority; I2S is fallback, CX31993 "
             "VID=0x%04X PID=0x%04X",
             CX31993_VID, CX31993_PID);
    return ESP_OK;
}

esp_err_t audio_output_set_sample_rate(uint32_t sample_rate)
{
    if (sample_rate < 8000 || sample_rate > 96000) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(i2s_output_set_sample_rate(sample_rate), TAG,
                        "I2S sample rate failed");

    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    s_sample_rate = sample_rate;
    if (s_device != NULL) {
        (void)start_usb_stream_locked(sample_rate);
    }
    xSemaphoreGive(s_output_mutex);
    (void)sb_play3_output_prepare(sample_rate);
    return ESP_OK;
}

void audio_output_get_status_text(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (sb_play3_output_is_active()) {
        snprintf(buffer, buffer_size, "PLAY3 USB %" PRIu32 "K",
                 s_sample_rate / 1000U);
        return;
    }

    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    const bool uac2_active = s_device != NULL && s_stream_active;
    const uint32_t usb_rate = s_usb_sample_rate;
    const uint8_t usb_bits = s_usb_bit_resolution;
    const bool is_cx31993 = s_usb_is_cx31993;
    const uint32_t i2s_rate = s_sample_rate;
    xSemaphoreGive(s_output_mutex);

    if (uac2_active) {
        snprintf(buffer, buffer_size, "%s USB %" PRIu32 "K %uB",
                 is_cx31993 ? "CX31993" : "UAC2", usb_rate / 1000U,
                 usb_bits);
    } else {
        snprintf(buffer, buffer_size, "I2S PCM5100A %" PRIu32 "K",
                 i2s_rate / 1000U);
    }
}

esp_err_t audio_output_write_s16(const int16_t *pcm, size_t frame_count,
                                 unsigned channels,
                                 unsigned volume_permille)
{
    if (pcm == NULL || frame_count == 0 ||
        (channels != 1 && channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume_permille > VOLUME_PERMILLE_MAX) {
        volume_permille = VOLUME_PERMILLE_MAX;
    }

    /* One playback task owns this buffer; static storage keeps it off the
     * comparatively small app/player call stacks. */
    static int32_t i2s_output[OUTPUT_BLOCK_FRAMES * 2];
    size_t offset = 0;
    while (offset < frame_count) {
        size_t frames = frame_count - offset;
        if (frames > OUTPUT_BLOCK_FRAMES) {
            frames = OUTPUT_BLOCK_FRAMES;
        }

        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t source = (offset + frame) * channels;
            const int32_t left_q31 = (int32_t)pcm[source] * 65536;
            const int32_t right_q31 = channels == 2
                                          ? (int32_t)pcm[source + 1] * 65536
                                          : left_q31;
            i2s_output[frame * 2] =
                scale_sample_q31(left_q31, volume_permille);
            i2s_output[frame * 2 + 1] =
                scale_sample_q31(right_q31, volume_permille);
        }

        audio_visualizer_submit_q31(i2s_output, frames);
        ESP_RETURN_ON_ERROR(write_stereo_q31(i2s_output, frames), TAG,
                            "audio output failed");
        offset += frames;
    }
    return ESP_OK;
}

esp_err_t audio_output_write_q31(const int32_t *pcm, size_t frame_count,
                                 unsigned channels,
                                 unsigned volume_permille)
{
    if (pcm == NULL || frame_count == 0 ||
        (channels != 1 && channels != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume_permille > VOLUME_PERMILLE_MAX) {
        volume_permille = VOLUME_PERMILLE_MAX;
    }

    static int32_t i2s_output[OUTPUT_BLOCK_FRAMES * 2];
    size_t offset = 0;
    while (offset < frame_count) {
        size_t frames = frame_count - offset;
        if (frames > OUTPUT_BLOCK_FRAMES) {
            frames = OUTPUT_BLOCK_FRAMES;
        }

        for (size_t frame = 0; frame < frames; ++frame) {
            const size_t source = (offset + frame) * channels;
            const int32_t left =
                scale_sample_q31(pcm[source], volume_permille);
            const int32_t right =
                channels == 2
                    ? scale_sample_q31(pcm[source + 1], volume_permille)
                    : left;
            i2s_output[frame * 2] = left;
            i2s_output[frame * 2 + 1] = right;
        }

        audio_visualizer_submit_q31(i2s_output, frames);
        ESP_RETURN_ON_ERROR(write_stereo_q31(i2s_output, frames), TAG,
                            "audio output failed");
        offset += frames;
    }
    return ESP_OK;
}

esp_err_t audio_output_silence(size_t frame_count)
{
    static const int32_t silence[OUTPUT_BLOCK_FRAMES * 2] = {0};
    while (frame_count > 0) {
        const size_t frames = frame_count > OUTPUT_BLOCK_FRAMES
                                  ? OUTPUT_BLOCK_FRAMES
                                  : frame_count;
        ESP_RETURN_ON_ERROR(
            audio_output_write_q31(silence, frames, 2, VOLUME_PERMILLE_MAX),
            TAG,
                            "silence write failed");
        frame_count -= frames;
    }
    return ESP_OK;
}

esp_err_t audio_output_test_tone(void)
{
    enum { TONE_HZ = 997, DURATION_MS = 250 };
    ESP_RETURN_ON_ERROR(audio_output_set_sample_rate(DEFAULT_SAMPLE_RATE), TAG,
                        "tone sample rate failed");

    static int16_t block[OUTPUT_BLOCK_FRAMES * 2];
    const unsigned total_frames = DEFAULT_SAMPLE_RATE * DURATION_MS / 1000;
    const unsigned half_period = DEFAULT_SAMPLE_RATE / TONE_HZ / 2;
    for (unsigned offset = 0; offset < total_frames;
         offset += OUTPUT_BLOCK_FRAMES) {
        unsigned frames = total_frames - offset;
        if (frames > OUTPUT_BLOCK_FRAMES) {
            frames = OUTPUT_BLOCK_FRAMES;
        }
        for (unsigned frame = 0; frame < frames; ++frame) {
            const int16_t value = ((offset + frame) / half_period) & 1
                                      ? -1800
                                      : 1800;
            block[frame * 2] = value;
            block[frame * 2 + 1] = value;
        }
        /* Keep the startup tone at half of its previous amplitude. */
        ESP_RETURN_ON_ERROR(audio_output_write_s16(block, frames, 2, 500), TAG,
                            "tone output failed");
    }
    return audio_output_silence(OUTPUT_BLOCK_FRAMES);
}
