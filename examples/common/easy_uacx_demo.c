/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "easy_uacx.h"
#ifndef CONFIG_EXAMPLE_LIFECYCLE_TEST
#define CONFIG_EXAMPLE_LIFECYCLE_TEST 0
#endif
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "euacx_demo";
unsigned easy_uacx_validate(euacx_port_t *port, const euacx_info_t *info, SemaphoreHandle_t stopped);
typedef struct { euacx_port_t *port; euacx_info_t info; } ready_t;
typedef struct {
    uint32_t rate, note_frames, frames;
    uint64_t byte;
    uint8_t bits, frame[8];
    float phase;
} tone_t;
static QueueHandle_t ready_queue;
static SemaphoreHandle_t stopped;
static _Atomic int stop_reason, stop_error;
static const float melody[16] = {261.63f,261.63f,392,392,440,440,392,392,349.23f,349.23f,329.63f,329.63f,293.66f,293.66f,261.63f,261.63f};
static int generate(euacx_port_t *port, void *buffer, size_t len, void *user)
{
    (void)port;
    tone_t *t = user;
    uint8_t *dst = buffer;
    unsigned sample = t->bits / 8, frame = sample * 2;
    uint64_t total = (uint64_t)t->frames * frame;
    if (t->byte == total) return EUACX_DATA_END;
    if (len > total - t->byte) len = total - t->byte;
    /* Work once per PCM frame. Per-byte 64-bit division starved the P4
     * producer at 24/32-bit 384 kHz despite successful USB completions. */
    size_t written = 0;
    while (written < len) {
        unsigned at = t->byte % frame;
        if (!at) {
            uint32_t f = t->byte / frame, slot = f / t->note_frames;
            int32_t value = 0;
            if (slot < 4u * CONFIG_EXAMPLE_PLAYBACK_BARS) {
                unsigned pos = f % t->note_frames;
                float attack = pos / (0.005f * t->rate), release = (t->note_frames - pos) / (0.03f * t->rate);
                float env = fminf(1, fminf(attack, release));
                value = (int32_t)(sinf(t->phase) * env * 0.2f * 2147483647.0f);
                t->phase += 6.2831853f * melody[slot] / t->rate;
                if (t->phase > 6.2831853f) t->phase -= 6.2831853f;
            }
            for (unsigned ch = 0; ch < 2; ++ch) for (unsigned b = 0; b < sample; ++b)
                t->frame[ch * sample + b] = (uint32_t)value >> (8 * (4 - sample + b));
        }
        size_t count = frame - at;
        if (count > len - written) count = len - written;
        memcpy(dst + written, t->frame + at, count);
        written += count;
        t->byte += count;
    }
    return len;
}
static void connected(euacx_port_t *port, const euacx_info_t *info, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "DAC %04x:%04x %s driver=%s verified=%u conn=%lu speed=%s volume=%u mute=%u range=[%d,%d]/%d",
        info->vid, info->pid, info->product, info->driver, info->verified, (unsigned long)info->conn_id,
        info->speed == EUACX_SPEED_HS ? "HS" : "FS", info->has_volume, info->has_mute,
        info->volume_min, info->volume_max, info->volume_res);
    for (unsigned b = 0; b < 3; ++b) for (unsigned r = 0; r < info->pcm[b].num_rates; ++r)
        ESP_LOGI(TAG, "CAP PCM bits=%u rate=%lu", 16 + 8 * b, (unsigned long)info->pcm[b].rates[r]);
    ESP_LOGI(TAG, "CAP DSD rates=%u (1.0 PCM only)", info->dsd.num_rates);
    ready_t ready = {.port = port, .info = *info};
    if (xQueueSend(ready_queue, &ready, 0) != pdTRUE) ESP_LOGW(TAG, "ready queue full");
}
static void disconnected(euacx_port_t *port, uint32_t conn, void *user)
{
    (void)port; (void)user;
    ESP_LOGI(TAG, "DAC disconnected conn=%lu", (unsigned long)conn);
}
static void stream_stopped(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t error, void *user)
{
    (void)port; (void)user;
    atomic_store(&stop_reason, reason); atomic_store(&stop_error, error);
    ESP_LOGI(TAG, "STOPPED reason=%d error=%s", reason, esp_err_to_name(error));
    xSemaphoreGive(stopped);
}
static bool same_connection(const ready_t *r)
{
    euacx_info_t info;
    return euacx_get_info(r->port, &info) == ESP_OK && info.conn_id == r->info.conn_id;
}
static bool play(const ready_t *r, uint8_t bits, uint32_t rate, bool pull)
{
    tone_t tone = {.rate = rate, .bits = bits, .note_frames = (uint32_t)(rate * 0.3f)};
    tone.frames = tone.note_frames * 4 * CONFIG_EXAMPLE_PLAYBACK_BARS + rate / 10;
    while (xSemaphoreTake(stopped, 0) == pdTRUE) {}
    euacx_stream_config_t cfg = {.format = EUACX_FORMAT_PCM, .sample_rate = rate, .bits = bits, .channels = 2,
        .on_data = pull ? generate : NULL, .data_user = &tone};
    esp_err_t error = euacx_stream_open(r->port, &cfg, NULL);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "RESULT bits=%u rate=%lu mode=%s status=FAIL open=%s", bits, (unsigned long)rate, pull ? "pull" : "push", esp_err_to_name(error));
        return false;
    }
    if (!pull) {
        uint8_t data[4093]; /* Deliberately not aligned to a PCM frame. */
        int n;
        while ((n = generate(r->port, data, sizeof(data), &tone)) > 0) {
            size_t offset = 0;
            while (offset < (size_t)n) {
                size_t written = 0;
                error = euacx_write(r->port, data + offset, n - offset, &written, 1000);
                offset += written;
                if (error != ESP_OK) break;
            }
            if (error != ESP_OK) break;
        }
        /* Push close stops immediately. Allow the final buffered audio to play. */
        /* Ring rounds up to a power of two and may grow for the USB pipeline.
         * A conservative tail also covers its in-flight ISO transfers. */
        if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(CONFIG_EUACX_BUFFER_MS * 2 + 1000));
        euacx_stream_close(r->port);
    }
    bool done = xSemaphoreTake(stopped, pdMS_TO_TICKS(60000)) == pdTRUE;
    if (!done) { euacx_stream_abort(r->port); euacx_stream_close(r->port); xSemaphoreTake(stopped, portMAX_DELAY); }
    bool ok = done && error == ESP_OK && same_connection(r) &&
        atomic_load(&stop_reason) == (pull ? EUACX_STOP_EOF : EUACX_STOP_CLOSED) && atomic_load(&stop_error) == ESP_OK;
    ESP_LOGI(TAG, "RESULT bits=%u rate=%lu mode=%s status=%s", bits, (unsigned long)rate, pull ? "pull" : "push", ok ? "PASS" : "FAIL");
    return ok;
}
typedef struct { uint32_t frames, rate; float phase; } soak_t;
static int soak_data(euacx_port_t *port, void *buffer, size_t len, void *user)
{
    (void)port;
    soak_t *s = user;
    if (!s->frames) return EUACX_DATA_END;
    unsigned frames = len / 6;
    if (frames > s->frames) frames = s->frames;
    uint8_t *data = buffer;
    for (unsigned i = 0; i < frames; ++i) {
        int32_t value = (int32_t)(sinf(s->phase) * 0.05f * 2147483647.0f);
        s->phase += 6.2831853f * 440 / s->rate;
        if (s->phase >= 6.2831853f) s->phase -= 6.2831853f;
        for (unsigned ch = 0; ch < 2; ++ch) for (unsigned b = 0; b < 3; ++b)
            data[i * 6 + ch * 3 + b] = (uint32_t)value >> (8 * (b + 1));
    }
    s->frames -= frames;
    return frames * 6;
}
static void soak(const ready_t *r)
{
    if (!CONFIG_EXAMPLE_SOAK_SECONDS || !same_connection(r)) return;
    uint32_t hz = 0;
    for (unsigned i = 0; i < r->info.pcm[1].num_rates; ++i) if (r->info.pcm[1].rates[i] == 48000) hz = 48000;
    if (!hz) return;
    soak_t tone = {.frames = (uint32_t)CONFIG_EXAMPLE_SOAK_SECONDS * hz, .rate = hz};
    euacx_stream_config_t cfg = {.format = EUACX_FORMAT_PCM, .sample_rate = hz, .bits = 24, .channels = 2, .on_data = soak_data, .data_user = &tone};
    while (xSemaphoreTake(stopped, 0) == pdTRUE) {}
    ESP_LOGI(TAG, "SOAK start rate=%lu seconds=%u", (unsigned long)hz, CONFIG_EXAMPLE_SOAK_SECONDS);
    esp_err_t e = euacx_stream_open(r->port, &cfg, NULL);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SOAK open failed: %s", esp_err_to_name(e)); return; }
    bool done = xSemaphoreTake(stopped, pdMS_TO_TICKS((CONFIG_EXAMPLE_SOAK_SECONDS + 10) * 1000)) == pdTRUE;
    if (!done) { euacx_stream_abort(r->port); euacx_stream_close(r->port); xSemaphoreTake(stopped, portMAX_DELAY); }
    ESP_LOGI(TAG, "SOAK status=%s reason=%d", done && atomic_load(&stop_reason) == EUACX_STOP_EOF ? "PASS" : "FAIL", atomic_load(&stop_reason));
}
void easy_uacx_demo_run(void)
{
    ready_queue = xQueueCreate(4, sizeof(ready_t)); stopped = xSemaphoreCreateBinary();
    assert(ready_queue && stopped);
    euacx_config_t config = EUACX_CONFIG_DEFAULT();
    config.cb = (euacx_callbacks_t){.on_connected = connected, .on_disconnected = disconnected, .on_stream_stopped = stream_stopped};
    size_t heap_before = esp_get_free_heap_size();
    bool lifecycle_done = false;
    ESP_ERROR_CHECK(euacx_init(&config));
    for (;;) {
        ready_t r; xQueueReceive(ready_queue, &r, portMAX_DELAY);
        if (!same_connection(&r)) continue;
        easy_uacx_validate(r.port, &r.info, stopped);
        unsigned passed = 0, failed = 0;
        for (unsigned b = 0; b < 3 && same_connection(&r); ++b) {
            unsigned format_passed = 0, format_failed = 0;
            bool verified[EUACX_MAX_RATES] = {0};
            for (unsigned rate = 0; rate < r.info.pcm[b].num_rates && same_connection(&r); ++rate) {
                bool pull = play(&r, 16 + 8 * b, r.info.pcm[b].rates[rate], true);
                bool push = same_connection(&r) && play(&r, 16 + 8 * b, r.info.pcm[b].rates[rate], false);
                format_passed += pull + push; format_failed += !pull + !push;
                verified[rate] = pull && push;
            }
            passed += format_passed; failed += format_failed;
            ESP_LOGI(TAG, "MATRIX PCM bits=%u passed=%u failed=%u", 16 + 8 * b, format_passed, format_failed);
            ESP_LOGI(TAG, "VERIFIED PCM bits=%u (transport PASS; confirm audibility and selected subslot before registering)", 16 + 8 * b);
            for (unsigned rate = 0; rate < r.info.pcm[b].num_rates; ++rate)
                if (verified[rate]) ESP_LOGI(TAG, "VERIFIED_RATE %lu", (unsigned long)r.info.pcm[b].rates[rate]);
        }
        ESP_LOGI(TAG, "SUMMARY passed=%u failed=%u conn=%lu", passed, failed, (unsigned long)r.info.conn_id);
        soak(&r);
        if (CONFIG_EXAMPLE_LIFECYCLE_TEST && !lifecycle_done) {
            ESP_ERROR_CHECK(euacx_deinit());
            vTaskDelay(pdMS_TO_TICKS(200));
            size_t after = esp_get_free_heap_size();
            ESP_LOGI(TAG, "LIFECYCLE heap_before=%u heap_after=%u delta=%d", (unsigned)heap_before, (unsigned)after, (int)after - (int)heap_before);
            for (unsigned cycle = 0; cycle < 3; ++cycle) {
                xQueueReset(ready_queue);
                ESP_ERROR_CHECK(euacx_init(&config));
                vTaskDelay(pdMS_TO_TICKS(100));
                ESP_ERROR_CHECK(euacx_deinit());
                vTaskDelay(pdMS_TO_TICKS(200));
                ESP_LOGI(TAG, "LIFECYCLE cycle=%u heap=%u", cycle + 1, (unsigned)esp_get_free_heap_size());
            }
            lifecycle_done = true; xQueueReset(ready_queue);
            ESP_ERROR_CHECK(euacx_init(&config));
        }
    }
}
