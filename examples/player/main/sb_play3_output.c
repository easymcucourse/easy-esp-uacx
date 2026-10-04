#include "sb_play3_output.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/uac_host.h"

#define SB_PLAY3_VID 0x041E
#define SB_PLAY3_PID 0x324D
#define OUTPUT_BLOCK_FRAMES 256U
#define DEVICE_TASK_STACK_SIZE 8192
#define DRIVER_TASK_PRIORITY 19
#define WRITE_TIMEOUT_MS 1000U

static const char *TAG = "sb_play3";

static SemaphoreHandle_t s_mutex;
static uac_host_device_handle_t s_device;
static bool s_stream_active;
static uint8_t s_bit_resolution;
static uint32_t s_sample_rate;
static uint32_t s_failed_sample_rate;
static TaskHandle_t s_device_task;
static portMUX_TYPE s_connect_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connecting;
static uint8_t s_pending_addr;
static uint8_t s_pending_iface;
static _Atomic bool s_disconnect_seen;

static void device_event_callback(uac_host_device_handle_t device,
                                  const uac_host_device_event_t event,
                                  void *argument)
{
    (void)device;
    (void)argument;
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Sound Blaster PLAY! 3 disconnected");
        atomic_store(&s_disconnect_seen, true);
        if (s_device_task != NULL) {
            xTaskNotifyGive(s_device_task);
        }
    } else if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "Sound Blaster PLAY! 3 transfer error");
    }
}

static void device_task(void *argument)
{
    (void)argument;
    uint8_t address;
    uint8_t interface_number;
    portENTER_CRITICAL(&s_connect_lock);
    address = s_pending_addr;
    interface_number = s_pending_iface;
    portEXIT_CRITICAL(&s_connect_lock);

    uac_host_device_handle_t device = NULL;
    const uac_host_device_config_t config = {
        .addr = address,
        .iface_num = interface_number,
        .buffer_size = 96U * 2U * 3U * 24U,
        .buffer_threshold = 96U * 2U * 3U * 8U,
        .callback = device_event_callback,
        .callback_arg = NULL,
    };
    /* Filter by VID/PID before the UAC1 parser claims an interface. The UAC1
     * discovery callback also sees UAC2 AudioStreaming interfaces. */
    esp_err_t error = uac_host_device_open_with_vid_pid(
        SB_PLAY3_VID, SB_PLAY3_PID, &config, &device);
    if (error != ESP_OK) {
        if (error != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "PLAY! 3 UAC1 open failed: %s",
                     esp_err_to_name(error));
        }
        goto done;
    }

    uac_host_dev_info_t info;
    error = uac_host_get_device_info(device, &info);
    if (error != ESP_OK || info.VID != SB_PLAY3_VID ||
        info.PID != SB_PLAY3_PID) {
        ESP_LOGI(TAG,
                 "UAC1 device %04X:%04X is not Sound Blaster PLAY! 3; ignored",
                 error == ESP_OK ? info.VID : 0,
                 error == ESP_OK ? info.PID : 0);
        goto done;
    }

    ESP_LOGI(TAG,
             "Sound Blaster PLAY! 3 detected: VID=0x%04X PID=0x%04X "
             "interface=%u alternates=%u",
             info.VID, info.PID, info.iface_num, info.iface_alt_num);
    (void)uac_host_printf_device_param(device);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_device = device;
    s_stream_active = false;
    s_failed_sample_rate = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "SB PLAY3 DRIVER LOAD PASS; waiting for playback format");

    while (!atomic_load(&s_disconnect_seen)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_device = NULL;
    const bool was_active = s_stream_active;
    s_stream_active = false;
    s_bit_resolution = 0;
    s_sample_rate = 0;
    s_failed_sample_rate = 0;
    xSemaphoreGive(s_mutex);
    if (was_active) {
        (void)uac_host_device_stop(device);
    }
    ESP_LOGI(TAG, "Sound Blaster PLAY! 3 removed; other output restored");

done:
    if (device != NULL) {
        (void)uac_host_device_close(device);
    }
    atomic_store(&s_disconnect_seen, false);
    portENTER_CRITICAL(&s_connect_lock);
    s_device_task = NULL;
    s_connecting = false;
    portEXIT_CRITICAL(&s_connect_lock);
    vTaskDelete(NULL);
}

static void driver_event_callback(uint8_t address, uint8_t interface_number,
                                  const uac_host_driver_event_t event,
                                  void *argument)
{
    (void)argument;
    if (event != UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
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

    const BaseType_t created = xTaskCreatePinnedToCore(
        device_task, "sb_play3_device", DEVICE_TASK_STACK_SIZE, NULL,
        DRIVER_TASK_PRIORITY + 1, &s_device_task, 0);
    if (created != pdPASS) {
        portENTER_CRITICAL(&s_connect_lock);
        s_connecting = false;
        s_device_task = NULL;
        portEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGE(TAG, "cannot create Sound Blaster PLAY! 3 device task");
    }
}

esp_err_t sb_play3_output_init(void)
{
    if (s_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uac_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = DRIVER_TASK_PRIORITY,
        .stack_size = 6144,
        .core_id = 0,
        .callback = driver_event_callback,
        .callback_arg = NULL,
    };
    const esp_err_t error = uac_host_install(&config);
    if (error == ESP_OK) {
        ESP_LOGI(TAG,
                 "Sound Blaster PLAY! 3 UAC1 detector ready, VID=0x%04X "
                 "PID=0x%04X",
                 SB_PLAY3_VID, SB_PLAY3_PID);
    }
    return error;
}

esp_err_t sb_play3_output_prepare(uint32_t sample_rate)
{
    if (s_mutex == NULL || sample_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_device == NULL) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_stream_active && s_sample_rate == sample_rate) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    if (!s_stream_active && s_failed_sample_rate == sample_rate) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_stream_active) {
        (void)uac_host_device_stop(s_device);
        s_stream_active = false;
    }

    uac_host_stream_config_t stream = {
        .channels = 2,
        .bit_resolution = 24,
        .sample_freq = sample_rate,
        .flags = 0,
    };
    esp_err_t error = uac_host_device_start(s_device, &stream);
    if (error != ESP_OK) {
        stream.bit_resolution = 16;
        error = uac_host_device_start(s_device, &stream);
    }
    if (error != ESP_OK) {
        s_failed_sample_rate = sample_rate;
        s_bit_resolution = 0;
        ESP_LOGW(TAG,
                 "PLAY! 3 does not support native %" PRIu32
                 " Hz stereo (%s); keeping other output",
                 sample_rate, esp_err_to_name(error));
        xSemaphoreGive(s_mutex);
        return error;
    }

    s_stream_active = true;
    s_sample_rate = sample_rate;
    s_bit_resolution = stream.bit_resolution;
    s_failed_sample_rate = 0;
    (void)uac_host_device_set_mute(s_device, false);
    (void)uac_host_device_set_volume(s_device, 100);
    ESP_LOGI(TAG,
             "SB PLAY3 AUDIO SELECTED: %" PRIu32
             " Hz native, %u-bit stereo; no sample-rate conversion",
             sample_rate, s_bit_resolution);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool sb_play3_output_is_active(void)
{
    if (s_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool active = s_device != NULL && s_stream_active;
    xSemaphoreGive(s_mutex);
    return active;
}

esp_err_t sb_play3_output_write_q31(const int32_t *stereo_pcm,
                                    size_t frame_count)
{
    if (stereo_pcm == NULL || frame_count == 0 ||
        frame_count > OUTPUT_BLOCK_FRAMES || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static uint8_t output[OUTPUT_BLOCK_FRAMES * 2U * 3U];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_device == NULL || !s_stream_active) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *destination = output;
    for (size_t index = 0; index < frame_count * 2U; ++index) {
        if (s_bit_resolution == 24) {
            const int32_t sample = stereo_pcm[index] >> 8;
            *destination++ = (uint8_t)(sample & 0xff);
            *destination++ = (uint8_t)((sample >> 8) & 0xff);
            *destination++ = (uint8_t)((sample >> 16) & 0xff);
        } else {
            const int16_t sample = (int16_t)(stereo_pcm[index] >> 16);
            *destination++ = (uint8_t)(sample & 0xff);
            *destination++ = (uint8_t)((sample >> 8) & 0xff);
        }
    }
    const uint32_t bytes =
        (uint32_t)(frame_count * 2U * (s_bit_resolution / 8U));
    const esp_err_t error = uac_host_device_write(
        s_device, output, bytes, pdMS_TO_TICKS(WRITE_TIMEOUT_MS));
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "PLAY! 3 write failed: %s", esp_err_to_name(error));
    }
    xSemaphoreGive(s_mutex);
    return error;
}
