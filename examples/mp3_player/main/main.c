/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
/*
 * easy-esp-uacx MP3 playback example
 * ==================================
 *
 * Decodes an MP3 file embedded in Flash (main/test.mp3) with Espressif's
 * esp_audio_codec and streams interleaved stereo PCM to an attached USB DAC
 * via easy-esp-uacx in pull mode:
 *
 *   Flash (test.mp3) -> esp_audio_simple_dec (MP3) -> PCM buffer -> easy-esp-uacx -> USB DAC
 *
 * Works on both ESP32-S3 (FS Host) and ESP32-P4 (HS Host):
 *   idf.py set-target esp32s3   # or esp32p4
 *   idf.py -p PORT flash monitor
 *
 * Replace main/test.mp3 with your own <1 MB MP3, or swap the Flash read in
 * mp3_read_chunk() for fread() to stream MP3 files from an SD card.
 */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "easy_uacx.h"
#include "esp_mp3_dec.h"
#include "esp_audio_simple_dec.h"

static const char *TAG = "euacx_mp3";

/* Embedded royalty-free MP3 in Flash (main/test.mp3 via EMBED_FILES). */
extern const uint8_t test_mp3_start[] asm("_binary_test_mp3_start");
extern const uint8_t test_mp3_end[]   asm("_binary_test_mp3_end");

typedef struct {
    euacx_port_t *port;
    euacx_info_t  info;
} ready_t;

typedef struct {
    const uint8_t                 *mp3_data;
    size_t                         mp3_size;
    size_t                         mp3_pos;
    uint8_t                        in_buf[1024];
    esp_audio_simple_dec_raw_t     raw;
    esp_audio_simple_dec_handle_t  dec;
    uint8_t                       *pcm_buf;
    size_t                         pcm_cap;
    size_t                         pcm_len;
    size_t                         pcm_pos;
    uint32_t                       sample_rate;
    uint8_t                        bits;
    uint8_t                        channels;
    bool                           info_ready;
    bool                           eof;
} mp3_player_t;

static QueueHandle_t     ready_queue;
static SemaphoreHandle_t stopped;
static _Atomic int       stop_reason;
static _Atomic int       stop_error;

static size_t skip_id3v2(const uint8_t *data, size_t size)
{
    if (size >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        size_t tag_size = ((size_t)(data[6] & 0x7f) << 21) |
                          ((size_t)(data[7] & 0x7f) << 14) |
                          ((size_t)(data[8] & 0x7f) << 7)  |
                          (size_t)(data[9] & 0x7f);
        size_t total = 10 + tag_size + ((data[5] & 0x10) ? 10 : 0);
        if (total < size) {
            return total;
        }
    }
    return 0;
}

static bool supports_pcm(const euacx_info_t *info, uint8_t bits, uint32_t rate)
{
    if (bits != 16 && bits != 24 && bits != 32) {
        return false;
    }
    const euacx_rate_list_t *list = &info->pcm[(bits - 16) / 8];
    for (unsigned i = 0; i < list->num_rates; ++i) {
        if (list->rates[i] == rate) {
            return true;
        }
    }
    return false;
}

/* Read the next chunk of MP3 bytes from Flash into dst (swap for fread() when using SD card). */
static size_t mp3_read_chunk(mp3_player_t *p, uint8_t *dst, size_t max_len)
{
    size_t remain = p->mp3_size - p->mp3_pos;
    size_t n = remain < max_len ? remain : max_len;
    if (n > 0) {
        memcpy(dst, p->mp3_data + p->mp3_pos, n);
        p->mp3_pos += n;
    }
    return n;
}

static bool decode_next_frame(mp3_player_t *p)
{
    while (!p->eof) {
        if (p->raw.len == 0) {
            size_t n = mp3_read_chunk(p, p->in_buf, sizeof(p->in_buf));
            if (n == 0) {
                p->eof = true;
                return false;
            }
            p->raw.buffer = p->in_buf;
            p->raw.len = (uint32_t)n;
            p->raw.consumed = 0;
            p->raw.eos = (p->mp3_pos >= p->mp3_size);
        }

        esp_audio_simple_dec_out_t out = {
            .buffer = p->pcm_buf,
            .len = (uint32_t)p->pcm_cap,
        };
        esp_audio_err_t ret = esp_audio_simple_dec_process(p->dec, &p->raw, &out);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            size_t need = (size_t)out.needed_size * 2;
            uint8_t *grown = realloc(p->pcm_buf, need);
            if (!grown) {
                ESP_LOGE(TAG, "OOM growing PCM buffer to %u bytes", (unsigned)need);
                p->eof = true;
                return false;
            }
            p->pcm_buf = grown;
            p->pcm_cap = need;
            continue;
        }

        uint32_t used = p->raw.consumed;
        if (used > p->raw.len) {
            used = p->raw.len;
        }
        p->raw.buffer += used;
        p->raw.len -= used;

        if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_DATA_LACK) {
            ESP_LOGW(TAG, "MP3 decode error: %d", ret);
            if (used == 0) {
                if (p->raw.len > 0) {
                    p->raw.buffer++;
                    p->raw.len--;
                } else {
                    p->eof = true;
                    return false;
                }
            }
            continue;
        }

        if (out.decoded_size > 0) {
            if (!p->info_ready) {
                esp_audio_simple_dec_info_t dec_info = {0};
                if (esp_audio_simple_dec_get_info(p->dec, &dec_info) == ESP_AUDIO_ERR_OK) {
                    p->sample_rate = dec_info.sample_rate;
                    p->bits = dec_info.bits_per_sample;
                    p->channels = dec_info.channel;
                    p->info_ready = true;
                }
            }
            if (p->channels == 1 && p->bits == 16) {
                size_t stereo_bytes = (size_t)out.decoded_size * 2;
                if (stereo_bytes > p->pcm_cap) {
                    uint8_t *grown = realloc(p->pcm_buf, stereo_bytes);
                    if (!grown) {
                        p->eof = true;
                        return false;
                    }
                    p->pcm_buf = grown;
                    p->pcm_cap = stereo_bytes;
                }
                int16_t *s = (int16_t *)p->pcm_buf;
                size_t samples = out.decoded_size / sizeof(int16_t);
                for (size_t i = samples; i-- > 0;) {
                    s[2 * i] = s[i];
                    s[2 * i + 1] = s[i];
                }
                p->pcm_len = stereo_bytes;
            } else {
                p->pcm_len = out.decoded_size;
            }
            p->pcm_pos = 0;
            return true;
        }

        if (used == 0) {
            /* Keep trailing partial bytes and top up from Flash so the parser sees a full frame. */
            if (p->raw.len > 0 && p->raw.buffer != p->in_buf) {
                memmove(p->in_buf, p->raw.buffer, p->raw.len);
            }
            p->raw.buffer = p->in_buf;
            size_t added = mp3_read_chunk(p, p->in_buf + p->raw.len, sizeof(p->in_buf) - p->raw.len);
            if (added == 0) {
                p->eof = true;
                return false;
            }
            p->raw.len += (uint32_t)added;
            p->raw.eos = (p->mp3_pos >= p->mp3_size);
        }
    }
    return false;
}

/* Pull callback invoked by easy-esp-uacx whenever its USB ring buffer needs PCM data. */
static int provide_pcm(euacx_port_t *port, void *buf, size_t len, void *user)
{
    (void)port;
    mp3_player_t *p = user;
    uint8_t *dst = buf;
    size_t total = 0;

    while (total < len) {
        if (p->pcm_pos == p->pcm_len) {
            if (!decode_next_frame(p)) {
                break;
            }
        }
        size_t avail = p->pcm_len - p->pcm_pos;
        size_t copy = len - total;
        if (copy > avail) {
            copy = avail;
        }
        memcpy(dst + total, p->pcm_buf + p->pcm_pos, copy);
        p->pcm_pos += copy;
        total += copy;
    }

    return total > 0 ? (int)total : EUACX_DATA_END;
}

static void on_connected(euacx_port_t *port, const euacx_info_t *info, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "DAC %04x:%04x %s driver=%s speed=%s",
             info->vid, info->pid, info->product, info->driver,
             info->speed == EUACX_SPEED_HS ? "HS" : "FS");
    for (unsigned b = 0; b < 3; ++b) {
        for (unsigned r = 0; r < info->pcm[b].num_rates; ++r) {
            ESP_LOGI(TAG, "  PCM %u-bit: %lu Hz", 16 + 8 * b, (unsigned long)info->pcm[b].rates[r]);
        }
    }
    ready_t ready = { .port = port, .info = *info };
    if (xQueueSend(ready_queue, &ready, 0) != pdTRUE) {
        ESP_LOGW(TAG, "ready queue full");
    }
}

static void on_disconnected(euacx_port_t *port, uint32_t conn_id, void *user)
{
    (void)port;
    (void)user;
    ESP_LOGI(TAG, "DAC disconnected (conn=%lu)", (unsigned long)conn_id);
}

static void on_stream_stopped(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t err, void *user)
{
    (void)port;
    (void)user;
    atomic_store(&stop_reason, reason);
    atomic_store(&stop_error, err);
    ESP_LOGI(TAG, "Stream stopped: reason=%d err=%s", reason, esp_err_to_name(err));
    xSemaphoreGive(stopped);
}

static bool same_connection(const ready_t *r)
{
    euacx_info_t cur;
    return euacx_get_info(r->port, &cur) == ESP_OK && cur.conn_id == r->info.conn_id;
}

static bool play_mp3_once(const ready_t *r)
{
    mp3_player_t *p = calloc(1, sizeof(*p));
    if (!p) {
        ESP_LOGE(TAG, "OOM allocating mp3_player_t");
        return false;
    }

    size_t mp3_len = (size_t)(test_mp3_end - test_mp3_start);
    size_t offset = skip_id3v2(test_mp3_start, mp3_len);
    p->mp3_data = test_mp3_start + offset;
    p->mp3_size = mp3_len - offset;
    p->pcm_cap = 9216; /* Holds one 1152-sample stereo frame (4608 B) plus mono->stereo headroom */
    p->pcm_buf = malloc(p->pcm_cap);
    if (!p->pcm_buf) {
        ESP_LOGE(TAG, "OOM allocating PCM buffer");
        free(p);
        return false;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .use_frame_dec = false,
    };
    esp_audio_err_t dec_ret = esp_audio_simple_dec_open(&dec_cfg, &p->dec);
    if (dec_ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_simple_dec_open failed: %d", dec_ret);
        free(p->pcm_buf);
        free(p);
        return false;
    }

    /* Decode the first frame so we know the real sample rate and bit depth before opening the stream. */
    bool ok = false;
    if (!decode_next_frame(p) || !p->info_ready) {
        ESP_LOGE(TAG, "Failed to decode first MP3 frame from Flash (%u bytes)", (unsigned)mp3_len);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Playing test.mp3 (%u bytes): %lu Hz, %u-bit, %u ch",
             (unsigned)mp3_len, (unsigned long)p->sample_rate, p->bits, p->channels);

    if (!supports_pcm(&r->info, p->bits, p->sample_rate)) {
        ESP_LOGW(TAG, "DAC does not support %lu Hz / %u-bit PCM; resample or switch DAC",
                 (unsigned long)p->sample_rate, p->bits);
        goto cleanup;
    }

    while (xSemaphoreTake(stopped, 0) == pdTRUE) {}

    euacx_stream_config_t stream = {
        .format = EUACX_FORMAT_PCM,
        .sample_rate = p->sample_rate,
        .bits = p->bits,
        .channels = 2,
        .on_data = provide_pcm,
        .data_user = p,
    };
    esp_err_t err = euacx_stream_open(r->port, &stream, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "euacx_stream_open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    /* Wait until the MP3 finishes draining (EOF) or the DAC is unplugged. */
    xSemaphoreTake(stopped, portMAX_DELAY);
    ok = (atomic_load(&stop_reason) == EUACX_STOP_EOF && atomic_load(&stop_error) == ESP_OK);

cleanup:
    esp_audio_simple_dec_close(p->dec);
    free(p->pcm_buf);
    free(p);
    return ok;
}

void app_main(void)
{
    ready_queue = xQueueCreate(4, sizeof(ready_t));
    stopped = xSemaphoreCreateBinary();
    assert(ready_queue && stopped);

    if (esp_mp3_dec_register() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to register MP3 decoder");
        return;
    }

    euacx_config_t config = EUACX_CONFIG_DEFAULT();
    config.cb.on_connected = on_connected;
    config.cb.on_disconnected = on_disconnected;
    config.cb.on_stream_stopped = on_stream_stopped;
    ESP_ERROR_CHECK(euacx_init(&config));

    ESP_LOGI(TAG, "Waiting for USB DAC (test.mp3 size: %u bytes in Flash)...",
             (unsigned)(test_mp3_end - test_mp3_start));

    for (;;) {
        ready_t r;
        xQueueReceive(ready_queue, &r, portMAX_DELAY);
        while (same_connection(&r)) {
            if (!play_mp3_once(&r)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
