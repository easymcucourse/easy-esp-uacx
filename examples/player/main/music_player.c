#include "music_player.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "audio_output.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "flac_playback.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define PLAYER_TASK_STACK_BYTES (48 * 1024)
#define PLAYER_TASK_PRIORITY    5
#define MP3_INPUT_BYTES         (16 * 1024)
#define WAV_RAW_BYTES           4096
#define DEFAULT_VOLUME_PERMILLE 175U
#define VOLUME_STEP_PERMILLE    50U
#define MAX_VOLUME_PERMILLE     1000U

enum {
    JUMP_NONE = 0,
    JUMP_NEXT = 1,
    JUMP_PREVIOUS = -1,
    JUMP_RESTART = 2,
};

static const char *TAG = "music_player";
static const music_library_t *s_library;
static TaskHandle_t s_player_task;
static portMUX_TYPE s_control_lock = portMUX_INITIALIZER_UNLOCKED;
static size_t s_track_index;
static unsigned s_volume_permille = DEFAULT_VOLUME_PERMILLE;
static bool s_paused;
static bool s_running;
static int s_pending_jump;
static char s_format_text[32] = "WAIT FORMAT";

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int current_jump(void)
{
    int jump;
    portENTER_CRITICAL(&s_control_lock);
    jump = s_pending_jump;
    portEXIT_CRITICAL(&s_control_lock);
    return jump;
}

static int take_jump(void)
{
    int jump;
    portENTER_CRITICAL(&s_control_lock);
    jump = s_pending_jump;
    s_pending_jump = JUMP_NONE;
    portEXIT_CRITICAL(&s_control_lock);
    return jump;
}

static unsigned current_volume(void)
{
    unsigned volume;
    portENTER_CRITICAL(&s_control_lock);
    volume = s_volume_permille;
    portEXIT_CRITICAL(&s_control_lock);
    return volume;
}

static bool current_paused(void)
{
    bool paused;
    portENTER_CRITICAL(&s_control_lock);
    paused = s_paused;
    portEXIT_CRITICAL(&s_control_lock);
    return paused;
}

static bool wait_for_playback_permission(void)
{
    while (current_paused() && current_jump() == JUMP_NONE) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return current_jump() == JUMP_NONE;
}

static void set_format_text(const char *format, uint32_t sample_rate,
                            unsigned bits_per_sample, unsigned channels,
                            unsigned bitrate_kbps)
{
    char text[sizeof(s_format_text)];
    const uint32_t khz = sample_rate / 1000U;
    const uint32_t khz_fraction = (sample_rate % 1000U) / 100U;
    if (bitrate_kbps > 0) {
        snprintf(text, sizeof(text), "%s %" PRIu32 ".%" PRIu32
                                     "K %uBIT %uK",
                 format, khz, khz_fraction, bits_per_sample, bitrate_kbps);
    } else {
        snprintf(text, sizeof(text), "%s %" PRIu32 ".%" PRIu32
                                     "K %uBIT %uCH",
                 format, khz, khz_fraction, bits_per_sample, channels);
    }
    portENTER_CRITICAL(&s_control_lock);
    strlcpy(s_format_text, text, sizeof(s_format_text));
    portEXIT_CRITICAL(&s_control_lock);
}

static void clear_format_text(void)
{
    portENTER_CRITICAL(&s_control_lock);
    strlcpy(s_format_text, "WAIT FORMAT", sizeof(s_format_text));
    portEXIT_CRITICAL(&s_control_lock);
}

static esp_err_t play_mp3(FILE *file)
{
    uint8_t *input = heap_caps_malloc(MP3_INPUT_BYTES, MALLOC_CAP_8BIT);
    mp3dec_t *decoder = calloc(1, sizeof(*decoder));
    int16_t *decoded = heap_caps_malloc(
        MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(*decoded), MALLOC_CAP_8BIT);
    if (input == NULL || decoder == NULL || decoded == NULL) {
        free(decoded);
        free(decoder);
        free(input);
        return ESP_ERR_NO_MEM;
    }

    mp3dec_init(decoder);
    size_t buffered = 0;
    bool end_of_file = false;
    unsigned decoded_frames = 0;
    esp_err_t result = ESP_OK;

    while (!end_of_file || buffered > 0) {
        if (!wait_for_playback_permission()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        if (!end_of_file && buffered < MP3_INPUT_BYTES) {
            const size_t received =
                fread(input + buffered, 1, MP3_INPUT_BYTES - buffered, file);
            buffered += received;
            if (received == 0) {
                end_of_file = true;
                if (ferror(file)) {
                    result = ESP_FAIL;
                    break;
                }
            }
        }

        if (buffered == 0) {
            break;
        }

        mp3dec_frame_info_t info = {0};
        const int samples = mp3dec_decode_frame(
            decoder, input, (int)buffered, decoded, &info);

        if (info.frame_bytes <= 0) {
            if (end_of_file) {
                break;
            }
            if (buffered == MP3_INPUT_BYTES) {
                /* Keep the sync tail while skipping an oversized ID3 block. */
                memmove(input, input + buffered - 4, 4);
                buffered = 4;
            }
            continue;
        }

        size_t consumed = (size_t)info.frame_bytes;
        if (consumed > buffered) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        buffered -= consumed;
        memmove(input, input + consumed, buffered);

        if (samples <= 0) {
            continue;
        }
        if ((info.channels != 1 && info.channels != 2) || info.hz <= 0) {
            result = ESP_ERR_NOT_SUPPORTED;
            break;
        }

        set_format_text("MP3", (uint32_t)info.hz, 16,
                        (unsigned)info.channels,
                        (unsigned)info.bitrate_kbps);
        result = audio_output_set_sample_rate((uint32_t)info.hz);
        if (result != ESP_OK) {
            break;
        }
        result = audio_output_write_s16(decoded, (size_t)samples,
                                        (unsigned)info.channels,
                                        current_volume());
        if (result != ESP_OK) {
            break;
        }
        ++decoded_frames;
    }

    if (result == ESP_OK && decoded_frames == 0) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "MP3 decoder processed %u frame(s)", decoded_frames);
    free(decoded);
    free(decoder);
    free(input);
    return result;
}

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint32_t data_size;
    long data_offset;
} wav_info_t;

static esp_err_t parse_wav(FILE *file, wav_info_t *info)
{
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_format = false;
    bool have_data = false;
    while (!have_data) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), file) !=
            sizeof(chunk_header)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t chunk_size = read_u32_le(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t format[16];
            if (chunk_size < sizeof(format) ||
                fread(format, 1, sizeof(format), file) != sizeof(format)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            info->format = read_u16_le(format);
            info->channels = read_u16_le(format + 2);
            info->sample_rate = read_u32_le(format + 4);
            info->block_align = read_u16_le(format + 12);
            info->bits_per_sample = read_u16_le(format + 14);
            const long remainder = (long)chunk_size - (long)sizeof(format);
            if (remainder > 0 && fseek(file, remainder, SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
            have_format = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->data_size = chunk_size;
            info->data_offset = ftell(file);
            have_data = true;
        } else if (fseek(file, (long)chunk_size, SEEK_CUR) != 0) {
            return ESP_FAIL;
        }

        if (!have_data && (chunk_size & 1U) != 0 &&
            fseek(file, 1, SEEK_CUR) != 0) {
            return ESP_FAIL;
        }
    }

    if (!have_format || info->format != 1 ||
        (info->channels != 1 && info->channels != 2) ||
        (info->bits_per_sample != 8 && info->bits_per_sample != 16 &&
         info->bits_per_sample != 24 && info->bits_per_sample != 32) ||
        info->block_align == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static int32_t wav_sample_to_q31(const uint8_t *sample, unsigned bits)
{
    switch (bits) {
    case 8:
        return (int32_t)((int)sample[0] - 128) * 16777216;
    case 16:
        return (int32_t)(int16_t)read_u16_le(sample) * 65536;
    case 24: {
        int32_t value = (int32_t)((uint32_t)sample[0] |
                                  ((uint32_t)sample[1] << 8) |
                                  ((uint32_t)sample[2] << 16));
        if ((value & 0x00800000) != 0) {
            value |= (int32_t)0xff000000;
        }
        return value * 256;
    }
    case 32:
        return (int32_t)read_u32_le(sample);
    default:
        return 0;
    }
}

static esp_err_t play_wav(FILE *file)
{
    wav_info_t info = {0};
    ESP_RETURN_ON_ERROR(parse_wav(file, &info), TAG,
                        "unsupported or corrupt WAV file");
    ESP_RETURN_ON_ERROR(audio_output_set_sample_rate(info.sample_rate), TAG,
                        "unsupported WAV sample rate");

    ESP_LOGI(TAG, "WAV: %lu Hz, %u channel(s), %u-bit PCM",
             (unsigned long)info.sample_rate, info.channels,
             info.bits_per_sample);
    set_format_text("WAV", info.sample_rate, info.bits_per_sample,
                    info.channels, 0);

    uint8_t raw[WAV_RAW_BYTES];
    /* Q1.31 preserves every valid bit from 8/16/24/32-bit integer WAV. */
    int32_t pcm[WAV_RAW_BYTES];
    uint32_t remaining = info.data_size;
    const unsigned bytes_per_sample = info.bits_per_sample / 8;
    esp_err_t result = ESP_OK;

    while (remaining >= info.block_align) {
        if (!wait_for_playback_permission()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }

        size_t requested = remaining < sizeof(raw) ? remaining : sizeof(raw);
        requested -= requested % info.block_align;
        const size_t received = fread(raw, 1, requested, file);
        if (received == 0) {
            result = ferror(file) ? ESP_FAIL : ESP_OK;
            break;
        }
        remaining -= (uint32_t)received;

        const size_t frames = received / info.block_align;
        const size_t sample_count = frames * info.channels;
        for (size_t index = 0; index < sample_count; ++index) {
            pcm[index] = wav_sample_to_q31(
                raw + index * bytes_per_sample, info.bits_per_sample);
        }
        result = audio_output_write_q31(pcm, frames, info.channels,
                                        current_volume());
        if (result != ESP_OK) {
            break;
        }
    }
    return result;
}

static bool flac_playback_allowed(void *context)
{
    (void)context;
    return wait_for_playback_permission();
}

static unsigned flac_current_volume(void *context)
{
    (void)context;
    return current_volume();
}

static void flac_format_ready(uint32_t sample_rate, unsigned channels,
                              unsigned bits_per_sample, void *context)
{
    (void)context;
    set_format_text("FLAC", sample_rate, bits_per_sample, channels, 0);
}

static esp_err_t play_track(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "cannot open %s: errno=%d", path, errno);
        return ESP_FAIL;
    }

    const char *extension = strrchr(path, '.');
    esp_err_t result;
    if (extension != NULL && strcasecmp(extension, ".mp3") == 0) {
        result = play_mp3(file);
    } else if (extension != NULL && strcasecmp(extension, ".wav") == 0) {
        result = play_wav(file);
    } else if (extension != NULL && strcasecmp(extension, ".flac") == 0) {
        result = flac_play_stream(file, flac_playback_allowed,
                                  flac_current_volume, flac_format_ready,
                                  NULL);
    } else {
        result = ESP_ERR_NOT_SUPPORTED;
    }

    fclose(file);
    audio_output_silence(256);
    return result;
}

static void player_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (s_library == NULL || s_library->count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        while (current_paused() && current_jump() == JUMP_NONE) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        portENTER_CRITICAL(&s_control_lock);
        const size_t index = s_track_index;
        s_running = true;
        portEXIT_CRITICAL(&s_control_lock);

        const char *path = music_library_track(s_library, index);
        clear_format_text();
        ESP_LOGI(TAG, "playing %u/%u: %s", (unsigned)(index + 1),
                 (unsigned)s_library->count, path);
        const esp_err_t result = play_track(path);
        int jump = take_jump();

        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "track failed: %s", esp_err_to_name(result));
        }
        if (jump == JUMP_NONE) {
            jump = JUMP_NEXT;
        }

        portENTER_CRITICAL(&s_control_lock);
        if (jump == JUMP_PREVIOUS) {
            s_track_index = s_track_index == 0 ? s_library->count - 1
                                               : s_track_index - 1;
        } else if (jump == JUMP_NEXT) {
            s_track_index = (s_track_index + 1) % s_library->count;
        }
        s_running = false;
        portEXIT_CRITICAL(&s_control_lock);
    }
}

esp_err_t music_player_start(const music_library_t *library)
{
    if (library == NULL || library->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_player_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_library = library;
    const BaseType_t created = xTaskCreate(
        player_task, "music_player", PLAYER_TASK_STACK_BYTES, NULL,
        PLAYER_TASK_PRIORITY, &s_player_task);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void music_player_set_track_index(size_t track_index)
{
    portENTER_CRITICAL(&s_control_lock);
    if (s_library == NULL || track_index < s_library->count) {
        s_track_index = track_index;
        strlcpy(s_format_text, "WAIT FORMAT", sizeof(s_format_text));
    }
    const size_t selected = s_track_index;
    portEXIT_CRITICAL(&s_control_lock);
    ESP_LOGW(TAG, "test start track index set to %u",
             (unsigned)(selected + 1));
}

void music_player_set_paused(bool paused)
{
    portENTER_CRITICAL(&s_control_lock);
    s_paused = paused;
    portEXIT_CRITICAL(&s_control_lock);
    ESP_LOGI(TAG, "%s", paused ? "startup paused" : "resumed");
}

void music_player_play_index(size_t track_index)
{
    portENTER_CRITICAL(&s_control_lock);
    if (s_library != NULL && track_index < s_library->count) {
        s_track_index = track_index;
        s_pending_jump = JUMP_RESTART;
        s_paused = false;
        s_running = false;
        strlcpy(s_format_text, "WAIT FORMAT", sizeof(s_format_text));
    }
    portEXIT_CRITICAL(&s_control_lock);
    ESP_LOGI(TAG, "web selected track %u", (unsigned)(track_index + 1U));
}

void music_player_toggle_pause(void)
{
    portENTER_CRITICAL(&s_control_lock);
    s_paused = !s_paused;
    const bool paused = s_paused;
    portEXIT_CRITICAL(&s_control_lock);
    ESP_LOGI(TAG, "%s", paused ? "paused" : "resumed");
}

static void request_jump(int jump)
{
    portENTER_CRITICAL(&s_control_lock);
    s_pending_jump = jump;
    s_paused = false;
    portEXIT_CRITICAL(&s_control_lock);
}

void music_player_next(void)
{
    request_jump(JUMP_NEXT);
}

void music_player_previous(void)
{
    request_jump(JUMP_PREVIOUS);
}

void music_player_restart(void)
{
    request_jump(JUMP_RESTART);
}

static void change_volume(int delta)
{
    portENTER_CRITICAL(&s_control_lock);
    int volume = (int)s_volume_permille + delta;
    if (volume < 0) {
        volume = 0;
    } else if (volume > MAX_VOLUME_PERMILLE) {
        volume = MAX_VOLUME_PERMILLE;
    }
    s_volume_permille = (unsigned)volume;
    portEXIT_CRITICAL(&s_control_lock);
    ESP_LOGI(TAG, "volume=%d.%d%%", volume / 10, volume % 10);
}

void music_player_volume_up(void)
{
    change_volume(VOLUME_STEP_PERMILLE);
}

void music_player_volume_down(void)
{
    change_volume(-(int)VOLUME_STEP_PERMILLE);
}

void music_player_set_volume_permille(unsigned volume_permille)
{
    if (volume_permille > MAX_VOLUME_PERMILLE) {
        volume_permille = MAX_VOLUME_PERMILLE;
    }
    portENTER_CRITICAL(&s_control_lock);
    s_volume_permille = volume_permille;
    portEXIT_CRITICAL(&s_control_lock);
    ESP_LOGI(TAG, "volume=%u.%u%%", volume_permille / 10U,
             volume_permille % 10U);
}

music_player_status_t music_player_get_status(void)
{
    music_player_status_t status;
    portENTER_CRITICAL(&s_control_lock);
    status.track_index = s_track_index;
    status.track_count = s_library != NULL ? s_library->count : 0;
    status.volume_permille = s_volume_permille;
    status.paused = s_paused;
    status.running = s_running;
    strlcpy(status.format_text, s_format_text, sizeof(status.format_text));
    portEXIT_CRITICAL(&s_control_lock);
    return status;
}
