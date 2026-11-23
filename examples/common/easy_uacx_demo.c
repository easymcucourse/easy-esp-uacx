/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
/*
 * easy_uacx playback demo
 * =======================
 *
 * This file shows the complete life of a USB DAC application built on
 * easy_uacx.h:
 *
 *   1. euacx_init()            start the driver (it installs USB Host for you)
 *   2. on_connected callback   a DAC was plugged in and its capabilities read
 *   3. euacx_stream_open()     open a stereo PCM stream at a bit depth / rate
 *   4. feed audio, either
 *        - pull mode: the driver calls your on_data callback when it needs
 *          more bytes (see generate() / soak_data()), or
 *        - push mode: your task calls euacx_write() (see the !pull branch
 *          in play())
 *   5. on_stream_stopped       the stream ended (EOF, close, unplug, error)
 *   6. euacx_deinit()          release everything (see the lifecycle test)
 *
 * The demo plays a short melody at EVERY bit depth and sample rate the DAC
 * advertises, in both pull and push mode, and logs PASS/FAIL per entry. A real
 * application usually needs only one format, so the minimal shape is:
 *
 *     euacx_config_t cfg = EUACX_CONFIG_DEFAULT();
 *     cfg.cb.on_connected = my_connected;          // copy info, wake your task
 *     cfg.cb.on_stream_stopped = my_stopped;
 *     ESP_ERROR_CHECK(euacx_init(&cfg));
 *     ... wait for my_connected ...
 *     euacx_stream_config_t s = {.format = EUACX_FORMAT_PCM, .sample_rate = 48000,
 *                                .bits = 16, .channels = 2,
 *                                .on_data = my_fill, .data_user = &my_state};
 *     ESP_ERROR_CHECK(euacx_stream_open(port, &s, NULL));
 *
 * PCM format reminder: interleaved L/R, signed, little-endian. 16-bit uses
 * 2 bytes per sample, 24-bit uses 3 PACKED bytes, 32-bit uses 4 bytes.
 */
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
/* Extra runtime self-checks (ownership/abort/volume rules); see easy_uacx_validation.c. */
unsigned easy_uacx_validate(euacx_port_t *port, const euacx_info_t *info, SemaphoreHandle_t stopped);

/* Message sent from the on_connected callback to the demo task. The info
 * pointer given to on_connected is only valid during the callback, so we copy
 * the whole struct by value. */
typedef struct { euacx_port_t *port; euacx_info_t info; } ready_t;

/* State of the melody generator. It is passed to the driver as data_user and
 * must stay alive until the stream has stopped (here it lives on play()'s
 * stack, and play() waits for on_stream_stopped before returning). */
typedef struct {
    uint32_t rate, note_frames, frames; /* sample rate, frames per note, total frames */
    uint64_t byte;                      /* bytes produced so far */
    uint8_t bits, frame[8];             /* bit depth, cached bytes of the current stereo frame */
    float phase;                        /* oscillator phase in radians */
} tone_t;

/* Callbacks run in the driver's callback task. Keep them short and hand work
 * over to your own task: here via a queue (connect) and a semaphore (stop). */
static QueueHandle_t ready_queue;
static SemaphoreHandle_t stopped;
static _Atomic int stop_reason, stop_error;

/* "Twinkle Twinkle Little Star", one bar = 4 notes. */
static const float melody[16] = {261.63f,261.63f,392,392,440,440,392,392,349.23f,349.23f,329.63f,329.63f,293.66f,293.66f,261.63f,261.63f};

/*
 * Pull-mode data callback (type euacx_data_cb_t).
 *
 * The driver's feed task calls this whenever its buffer has room. Fill up to
 * `len` bytes of PCM in the format given to euacx_stream_open() and return:
 *   - 1..len           number of bytes written to `buffer`
 *   - 0                no data right now, try again later (may cause underrun)
 *   - EUACX_DATA_END   no more audio; the driver drains what is buffered and
 *                      then reports EUACX_STOP_EOF via on_stream_stopped
 *
 * `len` and the returned count do NOT have to be a multiple of the frame size;
 * that is why this generator tracks a byte position. Return quickly, and do
 * not call euacx_write(), euacx_stream_open/close() on this port, or
 * euacx_deinit() from here.
 *
 * The same function is reused in push mode as a plain "fill this buffer"
 * helper, which is why it has the callback signature.
 */
static int generate(euacx_port_t *port, void *buffer, size_t len, void *user)
{
    (void)port;
    tone_t *t = user;
    uint8_t *dst = buffer;
    unsigned sample = t->bits / 8, frame = sample * 2;   /* bytes per sample / per stereo frame */
    uint64_t total = (uint64_t)t->frames * frame;
    if (t->byte == total) return EUACX_DATA_END;         /* everything delivered: signal EOF */
    if (len > total - t->byte) len = total - t->byte;    /* last chunk may be shorter */
    /* Work once per PCM frame. Per-byte 64-bit division starved the P4
     * producer at 24/32-bit 384 kHz despite successful USB completions. */
    size_t written = 0;
    while (written < len) {
        unsigned at = t->byte % frame;                   /* offset inside the current frame */
        if (!at) {
            /* Start of a new frame: compute one sample and cache its bytes. */
            uint32_t f = t->byte / frame, slot = f / t->note_frames;
            int32_t value = 0;                           /* silence after the melody */
            if (slot < 4u * CONFIG_EXAMPLE_PLAYBACK_BARS) {
                unsigned pos = f % t->note_frames;
                /* Short attack/release envelope to avoid clicks between notes. */
                float attack = pos / (0.005f * t->rate), release = (t->note_frames - pos) / (0.03f * t->rate);
                float env = fminf(1, fminf(attack, release));
                /* Full-scale 32-bit sample at 20% amplitude. */
                value = (int32_t)(sinf(t->phase) * env * 0.2f * 2147483647.0f);
                t->phase += 6.2831853f * melody[slot] / t->rate;
                if (t->phase > 6.2831853f) t->phase -= 6.2831853f;
            }
            /* Convert the 32-bit sample to the stream bit depth by keeping its
             * top `sample` bytes, stored little-endian, same value on L and R. */
            for (unsigned ch = 0; ch < 2; ++ch) for (unsigned b = 0; b < sample; ++b)
                t->frame[ch * sample + b] = (uint32_t)value >> (8 * (4 - sample + b));
        }
        /* Copy the rest of the current frame (or as much as fits). */
        size_t count = frame - at;
        if (count > len - written) count = len - written;
        memcpy(dst + written, t->frame + at, count);
        written += count;
        t->byte += count;
    }
    return len;
}

/*
 * on_connected: a DAC finished enumeration and is ready for streaming.
 * `info` lists what the DAC can play: info->pcm[0/1/2] are the supported
 * sample rates for 16/24/32-bit input. Copy anything you need; the pointer
 * is only valid during this call.
 */
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
    /* Hand the DAC over to the demo task instead of streaming from the callback. */
    ready_t ready = {.port = port, .info = *info};
    if (xQueueSend(ready_queue, &ready, 0) != pdTRUE) ESP_LOGW(TAG, "ready queue full");
}

/* on_disconnected: the DAC was unplugged (or released by euacx_deinit()). The
 * port handle itself stays valid; a later on_connected reuses it with a new
 * conn_id. */
static void disconnected(euacx_port_t *port, uint32_t conn, void *user)
{
    (void)port; (void)user;
    ESP_LOGI(TAG, "DAC disconnected conn=%lu", (unsigned long)conn);
}

/* on_stream_stopped: called exactly once for every successfully opened stream.
 * After this, the stream's data_user memory may be freed and a new stream may
 * be opened. We record why it stopped and wake the waiting demo task. */
static void stream_stopped(euacx_port_t *port, euacx_stop_reason_t reason, esp_err_t error, void *user)
{
    (void)port; (void)user;
    atomic_store(&stop_reason, reason); atomic_store(&stop_error, error);
    ESP_LOGI(TAG, "STOPPED reason=%d error=%s", reason, esp_err_to_name(error));
    xSemaphoreGive(stopped);
}

/* True if the DAC we were told about is still the one attached. conn_id
 * changes on every re-plug, so it detects "unplugged and plugged in again"
 * as well as "unplugged". */
static bool same_connection(const ready_t *r)
{
    euacx_info_t info;
    return euacx_get_info(r->port, &info) == ESP_OK && info.conn_id == r->info.conn_id;
}

/*
 * Play the melody once at the given format.
 *   pull = true  : driver pulls data through generate()      -> expect EUACX_STOP_EOF
 *   pull = false : this task pushes data with euacx_write()  -> expect EUACX_STOP_CLOSED
 * Returns true if the stream opened, played, and stopped as expected.
 */
static bool play(const ready_t *r, uint8_t bits, uint32_t rate, bool pull)
{
    tone_t tone = {.rate = rate, .bits = bits, .note_frames = (uint32_t)(rate * 0.3f)};  /* 300 ms per note */
    tone.frames = tone.note_frames * 4 * CONFIG_EXAMPLE_PLAYBACK_BARS + rate / 10;        /* + 100 ms tail of silence */
    while (xSemaphoreTake(stopped, 0) == pdTRUE) {}   /* clear any stale stop signal */

    /* Stream settings: zero-initialize, then set format/rate/bits/channels.
     * on_data != NULL selects pull mode; NULL selects push mode. sample_rate
     * must be one of info.pcm[(bits - 16) / 8].rates. */
    euacx_stream_config_t cfg = {.format = EUACX_FORMAT_PCM, .sample_rate = rate, .bits = bits, .channels = 2,
        .on_data = pull ? generate : NULL, .data_user = &tone};
    esp_err_t error = euacx_stream_open(r->port, &cfg, NULL);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "RESULT bits=%u rate=%lu mode=%s status=FAIL open=%s", bits, (unsigned long)rate, pull ? "pull" : "push", esp_err_to_name(error));
        return false;
    }
    /* Pull mode: nothing else to do here, the driver calls generate() on its
     * own task until it returns EUACX_DATA_END. */
    if (!pull) {
        /* Push mode: this task (the one that opened the stream) owns it and is
         * the only task allowed to call euacx_write() / euacx_stream_close(). */
        uint8_t data[4093]; /* Deliberately not aligned to a PCM frame. */
        int n;
        while ((n = generate(r->port, data, sizeof(data), &tone)) > 0) {
            size_t offset = 0;
            while (offset < (size_t)n) {
                /* euacx_write() copies bytes into the driver buffer, waiting up
                 * to 1000 ms for space. It may accept only part of the data
                 * (e.g. on timeout), so always advance by `written` and retry
                 * the remainder. */
                size_t written = 0;
                error = euacx_write(r->port, data + offset, n - offset, &written, 1000);
                offset += written;
                if (error != ESP_OK) break;   /* unplugged, aborted, or timed out */
            }
            if (error != ESP_OK) break;
        }
        /* Push close stops immediately. Allow the final buffered audio to play. */
        /* Ring rounds up to a power of two and may grow for the USB pipeline.
         * A conservative tail also covers its in-flight ISO transfers. */
        /* (There is no "drain" call in push mode: euacx_stream_close() discards
         * whatever is still buffered, so wait for it to play out first.) */
        if (error == ESP_OK) vTaskDelay(pdMS_TO_TICKS(CONFIG_EUACX_BUFFER_MS * 2 + 1000));
        euacx_stream_close(r->port);
    }
    /* Wait for on_stream_stopped. Only after it fires is `tone` (data_user)
     * no longer used by the driver, so it is safe to return. If something
     * hangs, abort + close forces the stream down. */
    bool done = xSemaphoreTake(stopped, pdMS_TO_TICKS(60000)) == pdTRUE;
    if (!done) { euacx_stream_abort(r->port); euacx_stream_close(r->port); xSemaphoreTake(stopped, portMAX_DELAY); }
    bool ok = done && error == ESP_OK && same_connection(r) &&
        atomic_load(&stop_reason) == (pull ? EUACX_STOP_EOF : EUACX_STOP_CLOSED) && atomic_load(&stop_error) == ESP_OK;
    ESP_LOGI(TAG, "RESULT bits=%u rate=%lu mode=%s status=%s", bits, (unsigned long)rate, pull ? "pull" : "push", ok ? "PASS" : "FAIL");
    return ok;
}

/* Optional long-running test (CONFIG_EXAMPLE_SOAK_SECONDS): a quiet 440 Hz
 * tone at 24-bit / 48 kHz in pull mode. soak_data() is the simplest possible
 * pull callback: it always returns whole frames (6 bytes = 2 ch x 3 bytes). */
typedef struct { uint32_t frames, rate; float phase; } soak_t;
static int soak_data(euacx_port_t *port, void *buffer, size_t len, void *user)
{
    (void)port;
    soak_t *s = user;
    if (!s->frames) return EUACX_DATA_END;        /* finished: drain and report EOF */
    unsigned frames = len / 6;                    /* whole 24-bit stereo frames that fit */
    if (frames > s->frames) frames = s->frames;
    uint8_t *data = buffer;
    for (unsigned i = 0; i < frames; ++i) {
        int32_t value = (int32_t)(sinf(s->phase) * 0.05f * 2147483647.0f);
        s->phase += 6.2831853f * 440 / s->rate;
        if (s->phase >= 6.2831853f) s->phase -= 6.2831853f;
        /* Packed 24-bit little-endian: bytes 1..3 of the 32-bit value. */
        for (unsigned ch = 0; ch < 2; ++ch) for (unsigned b = 0; b < 3; ++b)
            data[i * 6 + ch * 3 + b] = (uint32_t)value >> (8 * (b + 1));
    }
    s->frames -= frames;
    return frames * 6;
}
static void soak(const ready_t *r)
{
    if (!CONFIG_EXAMPLE_SOAK_SECONDS || !same_connection(r)) return;
    /* Only run if the DAC lists 48 kHz for 24-bit input (pcm[1]). */
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

#if CONFIG_EXAMPLE_CS43131_FS_TEST
/*
 * CS43131 (3302:98c2) Full Speed listening test (CONFIG_EXAMPLE_CS43131_FS_TEST).
 *
 * Its FS firmware accepts every rate on the UAC2 clock but only really plays
 * 44.1 kHz; transport passes everywhere, so only listening (or a recording)
 * shows the difference. Each case is announced on the serial log with the
 * expected sound, a ticker runs while it plays, and a gap separates cases.
 */
typedef struct { uint8_t bits; uint32_t rate; const char *expect; } cs43131_case_t;
static const cs43131_case_t cs43131_cases[] = {
    {16, 44100, "MELODY"},
    {24, 44100, "MELODY"},
    {32, 44100, "MELODY"},
    {16, 48000, "SILENT"},
    {16, 88200, "FAINT NOISE"},
    {16, 96000, "SILENT"},
    {24, 48000, "WHITE NOISE"},
    {24, 88200, "WHITE NOISE"},
    {24, 96000, "WHITE NOISE"},
    {32, 48000, "WHITE NOISE"},
    {16, 44100, "MELODY (recovers after the noise cases)"},
};
static bool cs43131_advertised(const euacx_info_t *info, uint8_t bits, uint32_t rate)
{
    const euacx_rate_list_t *list = &info->pcm[(bits - 16) / 8];
    for (unsigned i = 0; i < list->num_rates; ++i) if (list->rates[i] == rate) return true;
    return false;
}
static void cs43131_fs_test(const ready_t *r)
{
    const unsigned n = sizeof(cs43131_cases) / sizeof(cs43131_cases[0]);
    char result[sizeof(cs43131_cases) / sizeof(cs43131_cases[0])] = {0};   /* 'P' pass, 'F' fail, 'S' skipped */
    ESP_LOGW(TAG, "CS43131 FS LISTENING TEST: %u cases, pull mode, about %u s each", n, (unsigned)(CONFIG_EXAMPLE_PLAYBACK_BARS * 1.2f + 1));
    ESP_LOGW(TAG, "CS43131 the FS firmware only plays 44.1 kHz; the other cases are EXPECTED to be silent or white noise");
    if (r->info.has_volume) {
        int16_t volume = CONFIG_EXAMPLE_CS43131_TEST_VOLUME_DB * 256;
        esp_err_t e = euacx_set_volume(r->port, volume);
        ESP_LOGW(TAG, "CS43131 volume set to %d dB: %s", CONFIG_EXAMPLE_CS43131_TEST_VOLUME_DB, esp_err_to_name(e));
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (unsigned i = 0; i < n && same_connection(r); ++i) {
        const cs43131_case_t *c = &cs43131_cases[i];
        if (!cs43131_advertised(&r->info, c->bits, c->rate)) {
            result[i] = 'S';
            ESP_LOGW(TAG, "CS43131 CASE %u/%u PCM%u %lu Hz SKIPPED: not advertised (run the Cs43131 profile, verified caps off)",
                     i + 1, n, c->bits, (unsigned long)c->rate);
            continue;
        }
        ESP_LOGW(TAG, "========================================================");
        ESP_LOGW(TAG, "CS43131 CASE %u/%u  PCM%u  %lu Hz  expect=%s", i + 1, n, c->bits, (unsigned long)c->rate, c->expect);
        ESP_LOGW(TAG, "========================================================");
        tone_t tone = {.rate = c->rate, .bits = c->bits, .note_frames = (uint32_t)(c->rate * 0.3f)};
        tone.frames = tone.note_frames * 4 * CONFIG_EXAMPLE_PLAYBACK_BARS + c->rate / 10;
        while (xSemaphoreTake(stopped, 0) == pdTRUE) {}
        euacx_stream_config_t cfg = {.format = EUACX_FORMAT_PCM, .sample_rate = c->rate, .bits = c->bits, .channels = 2,
            .on_data = generate, .data_user = &tone};
        esp_err_t error = euacx_stream_open(r->port, &cfg, NULL);
        if (error != ESP_OK) {
            result[i] = 'F';
            ESP_LOGE(TAG, "CS43131 CASE %u/%u open failed: %s", i + 1, n, esp_err_to_name(error));
            continue;
        }
        /* Ticker: one line per second so the serial monitor shows what is playing right now. */
        unsigned seconds = 0;
        bool done = false;
        while (!done && seconds < 60) {
            done = xSemaphoreTake(stopped, pdMS_TO_TICKS(1000)) == pdTRUE;
            if (!done) ESP_LOGI(TAG, "CS43131 PLAYING %u/%u PCM%u %lu Hz t=%us expect=%s",
                                i + 1, n, c->bits, (unsigned long)c->rate, ++seconds, c->expect);
        }
        if (!done) { euacx_stream_abort(r->port); euacx_stream_close(r->port); xSemaphoreTake(stopped, portMAX_DELAY); }
        bool ok = done && atomic_load(&stop_reason) == EUACX_STOP_EOF && atomic_load(&stop_error) == ESP_OK;
        result[i] = ok ? 'P' : 'F';
        ESP_LOGW(TAG, "CS43131 CASE %u/%u done transport=%s (you should have heard: %s)", i + 1, n, ok ? "PASS" : "FAIL", c->expect);
        vTaskDelay(pdMS_TO_TICKS(1500));   /* gap so consecutive cases are easy to tell apart */
    }
    ESP_LOGW(TAG, "CS43131 SUMMARY (transport result; sound must be judged by ear or recording)");
    for (unsigned i = 0; i < n; ++i)
        ESP_LOGW(TAG, "CS43131   %2u  PCM%u %6lu Hz  %-4s  expect=%s", i + 1, cs43131_cases[i].bits,
                 (unsigned long)cs43131_cases[i].rate,
                 result[i] == 'P' ? "PASS" : result[i] == 'F' ? "FAIL" : result[i] == 'S' ? "SKIP" : "-", cs43131_cases[i].expect);
    ESP_LOGW(TAG, "CS43131 test finished; replug the dongle or reset the board to repeat");
}
#endif

/* Demo entry point, called from app_main(). Never returns. */
void easy_uacx_demo_run(void)
{
    ready_queue = xQueueCreate(4, sizeof(ready_t)); stopped = xSemaphoreCreateBinary();
    assert(ready_queue && stopped);

    /* Step 1: configure and start the driver. EUACX_CONFIG_DEFAULT() installs
     * the USB Host library for you; we only add our three callbacks (any of
     * them may be left NULL). euacx_init() returns right away and does not
     * wait for a DAC to be plugged in. */
    euacx_config_t config = EUACX_CONFIG_DEFAULT();
    config.cb = (euacx_callbacks_t){.on_connected = connected, .on_disconnected = disconnected, .on_stream_stopped = stream_stopped};
    size_t heap_before = esp_get_free_heap_size();
    bool lifecycle_done = false;
    ESP_ERROR_CHECK(euacx_init(&config));

    for (;;) {
        /* Step 2: block until on_connected reports a DAC. Skip stale entries
         * if the DAC was already unplugged again. */
        ready_t r; xQueueReceive(ready_queue, &r, portMAX_DELAY);
        if (!same_connection(&r)) continue;

#if CONFIG_EXAMPLE_CS43131_FS_TEST
        /* Dedicated listening test for the CS43131 at FS instead of the matrix. */
        if (r.info.vid == 0x3302 && r.info.pid == 0x98c2 && r.info.speed == EUACX_SPEED_FS) {
            cs43131_fs_test(&r);
            continue;
        }
#endif
        /* Library self-checks (not needed in your own application). */
        easy_uacx_validate(r.port, &r.info, stopped);

        /* Step 3: test matrix. For each bit depth (b = 0/1/2 -> 16/24/32-bit)
         * and each rate the DAC advertises, play once in pull and push mode. */
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

        /* Step 4 (optional, CONFIG_EXAMPLE_LIFECYCLE_TEST): shut the driver
         * down with euacx_deinit(), check that heap memory comes back, cycle
         * init/deinit a few times, then re-init so the DAC is detected again.
         * Port handles become invalid after euacx_deinit(); always get them
         * again from on_connected (or euacx_get_port()). */
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
        /* Loop back and wait for the next (re)connected DAC. */
    }
}
