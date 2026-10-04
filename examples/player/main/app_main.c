#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_output.h"
#include "board_pins.h"
#include "buttons.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music_library.h"
#include "music_player.h"
#include "sd_storage.h"
#include "st7789_display.h"
#include "web_server.h"
#include "wifi_ota.h"

#define MOUNT_POINT "/sdcard"

static const char *TAG = "music_app";
static sdmmc_card_t *s_card;
static music_library_t *s_library;
static bool s_display_ready;

static const char *base_filename(const char *path)
{
    if (path == NULL) {
        return "NO FILE";
    }
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void fit_filename(char *destination, size_t destination_size,
                         const char *path)
{
    const char *name = base_filename(path);
    const size_t length = strlen(name);
    if (destination == NULL || destination_size == 0) {
        return;
    }

    int pixel_width = 0;
    for (const unsigned char *cursor = (const unsigned char *)name;
         *cursor != '\0'; ++cursor) {
        if ((*cursor & 0xc0U) != 0x80U) {
            pixel_width += *cursor < 0x80U ? 8 : 16;
        }
    }
    if (pixel_width <= MUSIC_LCD_WIDTH - 16 && length < destination_size) {
        snprintf(destination, destination_size, "%s", name);
        return;
    }

    const char *start = name + length;
    pixel_width = 24; /* Three leading dots. */
    while (start > name) {
        const char *previous = start - 1;
        while (previous > name &&
               (((unsigned char)*previous & 0xc0U) == 0x80U)) {
            --previous;
        }
        const int character_width =
            (unsigned char)*previous < 0x80U ? 8 : 16;
        if (pixel_width + character_width > MUSIC_LCD_WIDTH - 16) {
            break;
        }
        pixel_width += character_width;
        start = previous;
    }
    snprintf(destination, destination_size, "...%s", start);
}

static void fit_media_info(char *destination, size_t destination_size,
                           const char *format_text)
{
    if (destination == NULL || destination_size == 0) {
        return;
    }
    if (format_text == NULL || format_text[0] == '\0') {
        snprintf(destination, destination_size, "WAIT FORMAT");
        return;
    }
    snprintf(destination, destination_size, "%s", format_text);
}

static void display_file_list(const music_player_status_t *status)
{
    if (!s_display_ready || status == NULL || status->track_count == 0) {
        return;
    }
    const size_t visible_count = status->track_count < 4 ? status->track_count : 4;
    size_t first = 0;
    if (status->track_count > visible_count) {
        if (status->track_index > 0) {
            first = status->track_index - 1;
        }
        if (first + visible_count > status->track_count) {
            first = status->track_count - visible_count;
        }
    }

    char names[4][96];
    const char *lines[4];
    for (size_t row = 0; row < visible_count; ++row) {
        fit_filename(names[row], sizeof(names[row]),
                     music_library_track(s_library, first + row));
        lines[row] = names[row];
    }
    st7789_display_update_file_list(lines, visible_count,
                                    status->track_index - first);
}

static void display_show(const char *status, const char *detail)
{
    if (!s_display_ready) {
        return;
    }
    const esp_err_t error = st7789_display_show(status, detail);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "display update failed: %s", esp_err_to_name(error));
    }
}

static void display_player_status(void)
{
    const music_player_status_t status = music_player_get_status();
    const char *path = status.track_count > 0
                           ? music_library_track(s_library, status.track_index)
                           : NULL;
    char ip_address[16];
    char sound_card[32];
    char filename[96];
    char media_info[32];
    wifi_ota_get_ip_string(ip_address, sizeof(ip_address));
    audio_output_get_status_text(sound_card, sizeof(sound_card));
    fit_filename(filename, sizeof(filename), path);
    fit_media_info(media_info, sizeof(media_info), status.format_text);
    st7789_display_show_player("ESP32S3 N16R8 SD20M", ip_address, sound_card,
                               filename, media_info);
    display_file_list(&status);
    st7789_display_update_cd(status.running && !status.paused);
}

static void configure_audio_power_sequence(void)
{
    ESP_ERROR_CHECK(gpio_reset_pin(MUSIC_POWER_INPUT_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(MUSIC_POWER_INPUT_GPIO,
                                       GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(MUSIC_POWER_INPUT_GPIO,
                                       GPIO_FLOATING));

    ESP_ERROR_CHECK(gpio_reset_pin(MUSIC_AUDIO_POWER_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(MUSIC_AUDIO_POWER_GPIO,
                                       GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(MUSIC_AUDIO_POWER_GPIO, 0));
    ESP_LOGI(TAG, "IO%d input configured; IO%d audio power LOW",
             MUSIC_POWER_INPUT_GPIO, MUSIC_AUDIO_POWER_GPIO);
    vTaskDelay(pdMS_TO_TICKS(MUSIC_AUDIO_POWER_PULSE_MS));
    ESP_ERROR_CHECK(gpio_set_level(MUSIC_AUDIO_POWER_GPIO, 1));
    ESP_LOGI(TAG,
             "IO%d audio power HIGH; wait %d ms before USB/I2S init/check",
             MUSIC_AUDIO_POWER_GPIO, MUSIC_AUDIO_POWER_SETTLE_MS);
}

static void print_help(void)
{
    printf("\nESP32-S3 CX31993 music player controls:\n"
           "  p / Space : play or pause\n"
           "  n         : next track\n"
           "  b         : previous track\n"
           "  r         : restart current track\n"
           "  + / -     : volume up / down\n"
           "  s         : status\n"
           "  l         : list tracks\n"
           "  h / ?     : help\n\n");
}

static void print_status(void)
{
    const music_player_status_t status = music_player_get_status();
    const char *path = status.track_count > 0
                           ? music_library_track(s_library, status.track_index)
                           : NULL;
    ESP_LOGI(TAG, "status: track=%u/%u, volume=%u.%u%%, %s, file=%s",
             (unsigned)(status.track_index + (status.track_count > 0)),
             (unsigned)status.track_count, status.volume_permille / 10,
             status.volume_permille % 10,
             status.paused ? "paused" : (status.running ? "playing" : "idle"),
             path != NULL ? path : "none");
}

static void list_tracks(void)
{
    for (size_t index = 0; index < s_library->count; ++index) {
        ESP_LOGI(TAG, "%3u: %s", (unsigned)(index + 1),
                 music_library_track(s_library, index));
    }
}

static void console_task(void *argument)
{
    (void)argument;
    setvbuf(stdin, NULL, _IONBF, 0);
    const int flags = fcntl(fileno(stdin), F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fileno(stdin), F_SETFL, flags | O_NONBLOCK);
    }
    print_help();

    for (;;) {
        const int character = fgetc(stdin);
        if (character == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        switch (character) {
        case 'p':
        case 'P':
        case ' ':
            music_player_toggle_pause();
            vTaskDelay(pdMS_TO_TICKS(20));
            display_player_status();
            break;
        case 'n':
        case 'N':
            music_player_next();
            vTaskDelay(pdMS_TO_TICKS(20));
            display_player_status();
            break;
        case 'b':
        case 'B':
            music_player_previous();
            vTaskDelay(pdMS_TO_TICKS(20));
            display_player_status();
            break;
        case 'r':
        case 'R':
            music_player_restart();
            vTaskDelay(pdMS_TO_TICKS(20));
            display_player_status();
            break;
        case '+':
        case '=':
            music_player_volume_up();
            vTaskDelay(pdMS_TO_TICKS(20));
            {
                const music_player_status_t status = music_player_get_status();
                char detail[24];
                snprintf(detail, sizeof(detail), "%u.%u PERCENT",
                         status.volume_permille / 10,
                         status.volume_permille % 10);
                display_show("VOLUME", detail);
            }
            break;
        case '-':
        case '_':
            music_player_volume_down();
            vTaskDelay(pdMS_TO_TICKS(20));
            {
                const music_player_status_t status = music_player_get_status();
                char detail[24];
                snprintf(detail, sizeof(detail), "%u.%u PERCENT",
                         status.volume_permille / 10,
                         status.volume_permille % 10);
                display_show("VOLUME", detail);
            }
            break;
        case 's':
        case 'S':
            print_status();
            display_player_status();
            break;
        case 'l':
        case 'L':
            list_tracks();
            break;
        case 'h':
        case 'H':
        case '?':
            print_help();
            break;
        default:
            break;
        }
    }
}

static void display_task(void *argument)
{
    (void)argument;
    char last_ip[16] = {0};
    char last_sound_card[32] = {0};
    char last_filename[96] = {0};
    char last_media_info[32] = {0};
    size_t last_track_index = (size_t)-1;
    size_t last_track_count = (size_t)-1;
    bool last_playing = false;
    TickType_t next_refresh = xTaskGetTickCount();

    for (;;) {
        const music_player_status_t status = music_player_get_status();
        const char *path = status.track_count > 0
                               ? music_library_track(s_library,
                                                     status.track_index)
                               : NULL;
        char current_ip[16];
        char current_sound_card[32];
        char current_filename[96];
        char current_media_info[32];
        wifi_ota_get_ip_string(current_ip, sizeof(current_ip));
        audio_output_get_status_text(current_sound_card, sizeof(current_sound_card));
        fit_filename(current_filename, sizeof(current_filename), path);
        fit_media_info(current_media_info, sizeof(current_media_info),
                       status.format_text);

        if (strcmp(current_ip, last_ip) != 0 ||
            strcmp(current_sound_card, last_sound_card) != 0) {
            strlcpy(last_ip, current_ip, sizeof(last_ip));
            strlcpy(last_sound_card, current_sound_card, sizeof(last_sound_card));
            st7789_display_update_status_bar("ESP32S3 N16R8 SD20M", last_ip, last_sound_card);
        }
        if (strcmp(current_filename, last_filename) != 0 ||
            strcmp(current_media_info, last_media_info) != 0) {
            strlcpy(last_filename, current_filename, sizeof(last_filename));
            strlcpy(last_media_info, current_media_info,
                    sizeof(last_media_info));
            st7789_display_update_track_info(last_filename, last_media_info);
        }
        if (status.track_index != last_track_index ||
            status.track_count != last_track_count) {
            last_track_index = status.track_index;
            last_track_count = status.track_count;
            display_file_list(&status);
        }

        const bool playing = status.running && !status.paused;
        if (playing || playing != last_playing) {
            st7789_display_update_cd(playing);
        }
        last_playing = playing;
        xTaskDelayUntil(&next_refresh, pdMS_TO_TICKS(125));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 SDSPI + CX31993 USB music player starting");
    const esp_err_t display_error = st7789_display_init();
    if (display_error == ESP_OK) {
        s_display_ready = true;
        display_show("MUSIC PLAYER", "STARTING");
    } else {
        ESP_LOGW(TAG, "ST7789 unavailable; continuing without display: %s",
                 esp_err_to_name(display_error));
    }
    ESP_LOGI(TAG,
             "I2S sound card: WS=%d DIN=%d BCK=%d MCLK=%d SD=%d; "
             "USB priority playback D-=GPIO19 D+=GPIO20",
             MUSIC_I2S_PIN_WS, MUSIC_I2S_PIN_DOUT, MUSIC_I2S_PIN_BCLK,
             MUSIC_I2S_PIN_MCLK, MUSIC_I2S_PIN_SD);
    ESP_LOGI(TAG,
             "SDSPI: CS=%d MOSI=%d CLK=%d MISO=%d; protocol mode=0",
             MUSIC_SD_PIN_CS, MUSIC_SD_PIN_MOSI,
             MUSIC_SD_PIN_CLK, MUSIC_SD_PIN_MISO);

    configure_audio_power_sequence();
    display_show("AUDIO POWER", "WAIT 10 SEC");
    vTaskDelay(pdMS_TO_TICKS(MUSIC_AUDIO_POWER_SETTLE_MS));

    const esp_err_t wifi_error = wifi_ota_start();
    if (wifi_error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi/OTA startup failed: %s",
                 esp_err_to_name(wifi_error));
    }

    ESP_ERROR_CHECK(audio_output_init());

#if MUSIC_ENABLE_BOOT_TONE
    ESP_ERROR_CHECK_WITHOUT_ABORT(audio_output_test_tone());
#endif

    display_show("SD MOUNT", "FIXED 20 MHZ");
    ESP_LOGI(TAG, "SDSPI speed sweep disabled; mounting at fixed %d kHz",
             MUSIC_SD_RUN_FREQ_KHZ);
    while (sd_storage_mount(MOUNT_POINT, MUSIC_SD_RUN_FREQ_KHZ, &s_card) !=
           ESP_OK) {
        display_show("SD ERROR", "FORMAT FAT32");
        ESP_LOGW(TAG,
                 "retrying fixed-speed SDSPI FAT mount in 3 seconds; "
                 "CX31993 hot-plug remains active");
        vTaskDelay(pdMS_TO_TICKS(3000));
        display_show("SD MOUNT", "20 MHZ RETRY");
    }

    s_library = heap_caps_calloc(1, sizeof(*s_library),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_library == NULL) {
        s_library = calloc(1, sizeof(*s_library));
    }
    if (s_library == NULL) {
        ESP_LOGE(TAG, "cannot allocate music library");
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(buttons_init());

    display_show("SD READY", "SCANNING MUSIC");
    ESP_ERROR_CHECK(music_library_scan(s_library, MOUNT_POINT));
    if (s_library->count == 0) {
        display_show("NO MUSIC", "ADD MP3 WAV FLAC");
        ESP_LOGE(TAG, "no MP3, WAV, or FLAC files found on the SD card");
        return;
    }

    music_player_set_paused(true);
    ESP_ERROR_CHECK(music_player_start(s_library));
    ESP_ERROR_CHECK_WITHOUT_ABORT(web_server_start(s_library));
    vTaskDelay(pdMS_TO_TICKS(20));
    display_player_status();

    if (s_display_ready &&
        xTaskCreatePinnedToCore(display_task, "lcd_status", 4096, NULL, 2, NULL, 0) !=
            pdPASS) {
        ESP_LOGE(TAG, "cannot start LCD status task");
    }
    if (xTaskCreate(console_task, "music_console", 4096, NULL, 3, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "cannot start console task");
    }
}
