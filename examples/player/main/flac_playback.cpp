#include "flac_playback.h"

#include <stdint.h>
#include <memory>
#include <cstring>

#include "audio_output.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "micro_flac/flac_decoder.h"

using micro_flac::FLAC_DECODER_END_OF_STREAM;
using micro_flac::FLAC_DECODER_HEADER_READY;
using micro_flac::FLAC_DECODER_NEED_MORE_DATA;
using micro_flac::FLAC_DECODER_SUCCESS;
using micro_flac::FLACDecoder;
using micro_flac::FLACDecoderResult;

namespace {

constexpr size_t FLAC_INPUT_BUFFER_BYTES = 64 * 1024;
constexpr size_t FLAC_INPUT_LOW_WATER_BYTES = 16 * 1024;
constexpr size_t FLAC_PCM_RING_FRAMES = 96 * 1024;
constexpr size_t FLAC_PCM_PREFILL_FRAMES = 24 * 1024;
constexpr size_t FLAC_OUTPUT_BLOCK_FRAMES = 256;
constexpr uint32_t FLAC_WAIT_MS = 10;
constexpr uint32_t FLAC_TASK_STACK_BYTES = 24 * 1024;
constexpr UBaseType_t FLAC_TASK_PRIORITY = 4;
const char *TAG = "flac_playback";

struct HeapCapsDeleter {
    void operator()(void *pointer) const { heap_caps_free(pointer); }
};

bool refill_input(FILE *file, uint8_t *buffer, size_t buffer_size,
                  size_t &offset, size_t &length)
{
    if (offset > 0 && length > 0) {
        memmove(buffer, buffer + offset, length);
    }
    offset = 0;
    const size_t space = buffer_size - length;
    if (space == 0) {
        return true;
    }
    const size_t received = fread(buffer + length, 1, space, file);
    length += received;
    return received > 0;
}

void refill_when_low(FILE *file, uint8_t *buffer, size_t buffer_size,
                     size_t &offset, size_t &length, bool &eof)
{
    if (!eof && length < FLAC_INPUT_LOW_WATER_BYTES) {
        eof = !refill_input(file, buffer, buffer_size, offset, length);
    }
}

struct PcmRing {
    int32_t *samples = nullptr;
    size_t capacity_frames = 0;
    size_t read_frame = 0;
    size_t write_frame = 0;
    size_t filled_frames = 0;
    bool closed = false;
    bool cancelled = false;
    uint32_t underruns = 0;
    SemaphoreHandle_t mutex = nullptr;
};

size_t min_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

esp_err_t ring_init(PcmRing &ring, size_t capacity_frames)
{
    ring.samples = static_cast<int32_t *>(heap_caps_malloc_prefer(
        capacity_frames * 2 * sizeof(*ring.samples), 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (ring.samples == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ring.capacity_frames = capacity_frames;
    ring.mutex = xSemaphoreCreateMutex();
    if (ring.mutex == nullptr) {
        heap_caps_free(ring.samples);
        ring.samples = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ring_destroy(PcmRing &ring)
{
    if (ring.mutex != nullptr) {
        vSemaphoreDelete(ring.mutex);
    }
    heap_caps_free(ring.samples);
}

void ring_close(PcmRing &ring)
{
    xSemaphoreTake(ring.mutex, portMAX_DELAY);
    ring.closed = true;
    xSemaphoreGive(ring.mutex);
}

void ring_cancel(PcmRing &ring)
{
    xSemaphoreTake(ring.mutex, portMAX_DELAY);
    ring.cancelled = true;
    xSemaphoreGive(ring.mutex);
}

bool ring_is_cancelled(PcmRing &ring)
{
    xSemaphoreTake(ring.mutex, portMAX_DELAY);
    const bool cancelled = ring.cancelled;
    xSemaphoreGive(ring.mutex);
    return cancelled;
}

size_t ring_filled_frames(PcmRing &ring)
{
    xSemaphoreTake(ring.mutex, portMAX_DELAY);
    const size_t filled = ring.filled_frames;
    xSemaphoreGive(ring.mutex);
    return filled;
}

esp_err_t ring_write(PcmRing &ring, const int32_t *pcm, size_t frame_count,
                     unsigned channels,
                     flac_playback_allowed_cb_t playback_allowed,
                     void *context)
{
    size_t offset = 0;
    while (offset < frame_count) {
        if (!playback_allowed(context)) {
            return ESP_ERR_INVALID_STATE;
        }

        xSemaphoreTake(ring.mutex, portMAX_DELAY);
        if (ring.cancelled) {
            xSemaphoreGive(ring.mutex);
            return ESP_ERR_INVALID_STATE;
        }
        const size_t space = ring.capacity_frames - ring.filled_frames;
        if (space == 0) {
            xSemaphoreGive(ring.mutex);
            vTaskDelay(pdMS_TO_TICKS(FLAC_WAIT_MS));
            continue;
        }

        size_t frames = min_size(frame_count - offset, space);
        frames = min_size(frames, ring.capacity_frames - ring.write_frame);
        int32_t *destination = ring.samples + ring.write_frame * 2;
        if (channels == 2) {
            memcpy(destination, pcm + offset * 2,
                   frames * 2 * sizeof(*destination));
        } else {
            for (size_t frame = 0; frame < frames; ++frame) {
                const int32_t sample = pcm[offset + frame];
                destination[frame * 2] = sample;
                destination[frame * 2 + 1] = sample;
            }
        }
        ring.write_frame = (ring.write_frame + frames) % ring.capacity_frames;
        ring.filled_frames += frames;
        xSemaphoreGive(ring.mutex);
        offset += frames;
    }
    return ESP_OK;
}

esp_err_t ring_read(PcmRing &ring, int32_t *pcm, size_t max_frames,
                    size_t &frames_read)
{
    frames_read = 0;
    bool counted_underrun = false;
    while (true) {
        xSemaphoreTake(ring.mutex, portMAX_DELAY);
        if (ring.cancelled) {
            xSemaphoreGive(ring.mutex);
            return ESP_ERR_INVALID_STATE;
        }
        if (ring.filled_frames > 0) {
            size_t frames = min_size(max_frames, ring.filled_frames);
            frames = min_size(frames, ring.capacity_frames - ring.read_frame);
            memcpy(pcm, ring.samples + ring.read_frame * 2,
                   frames * 2 * sizeof(*pcm));
            ring.read_frame = (ring.read_frame + frames) % ring.capacity_frames;
            ring.filled_frames -= frames;
            frames_read = frames;
            xSemaphoreGive(ring.mutex);
            return ESP_OK;
        }
        if (ring.closed) {
            xSemaphoreGive(ring.mutex);
            return ESP_OK;
        }
        if (!counted_underrun) {
            ++ring.underruns;
            counted_underrun = true;
        }
        xSemaphoreGive(ring.mutex);
        vTaskDelay(pdMS_TO_TICKS(FLAC_WAIT_MS));
    }
}

struct FlacDecodeContext {
    FILE *file = nullptr;
    flac_playback_allowed_cb_t playback_allowed = nullptr;
    void *callback_context = nullptr;
    PcmRing *ring = nullptr;
    SemaphoreHandle_t header_done = nullptr;
    SemaphoreHandle_t task_done = nullptr;
    esp_err_t result = ESP_FAIL;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t bits_per_sample = 0;
    uint32_t max_block_size = 0;
    uint64_t decoded_frames = 0;
    bool header_ready = false;
};

void flac_decode_task(void *argument)
{
    auto *ctx = static_cast<FlacDecodeContext *>(argument);
    FLACDecoder decoder;
    decoder.set_crc_check_enabled(false);

    std::unique_ptr<uint8_t, HeapCapsDeleter> input(
        static_cast<uint8_t *>(heap_caps_malloc_prefer(
            FLAC_INPUT_BUFFER_BYTES, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    std::unique_ptr<int32_t, HeapCapsDeleter> pcm;
    size_t pcm_capacity = 0;
    if (!input) {
        ctx->result = ESP_ERR_NO_MEM;
        xSemaphoreGive(ctx->header_done);
        ring_close(*ctx->ring);
        xSemaphoreGive(ctx->task_done);
        vTaskDelete(nullptr);
    }

    size_t offset = 0;
    size_t length = 0;
    uint32_t decoded_blocks_since_yield = 0;
    bool eof = false;
    bool header_notified = false;
    esp_err_t result = ESP_OK;

    while (result == ESP_OK && !ring_is_cancelled(*ctx->ring)) {
        if (!ctx->playback_allowed(ctx->callback_context)) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        if (length == 0 && !eof) {
            eof = !refill_input(ctx->file, input.get(), FLAC_INPUT_BUFFER_BYTES,
                                offset, length);
        }
        if (length == 0 && eof) {
            break;
        }

        size_t consumed = 0;
        size_t samples_decoded = 0;
        const FLACDecoderResult decode_result = decoder.decode(
            input.get() + offset, length, pcm.get(), pcm_capacity, consumed,
            samples_decoded);
        offset += consumed;
        length -= consumed;

        if (decode_result == FLAC_DECODER_HEADER_READY) {
            const auto &info = decoder.get_stream_info();
            ctx->sample_rate = info.sample_rate();
            ctx->channels = info.num_channels();
            ctx->bits_per_sample = info.bits_per_sample();
            ctx->max_block_size = info.max_block_size();
            if ((ctx->channels != 1 && ctx->channels != 2) ||
                ctx->sample_rate == 0 || ctx->bits_per_sample == 0 ||
                ctx->bits_per_sample > 32) {
                ESP_LOGE(TAG, "unsupported FLAC: %lu Hz, %lu ch, %lu-bit",
                         static_cast<unsigned long>(ctx->sample_rate),
                         static_cast<unsigned long>(ctx->channels),
                         static_cast<unsigned long>(ctx->bits_per_sample));
                result = ESP_ERR_NOT_SUPPORTED;
                break;
            }

            const size_t output_samples =
                decoder.get_output_buffer_size_samples();
            if (output_samples == 0) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            pcm.reset(static_cast<int32_t *>(heap_caps_malloc_prefer(
                output_samples * sizeof(int32_t), 2,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            if (!pcm) {
                result = ESP_ERR_NO_MEM;
                break;
            }
            pcm_capacity = output_samples;
            ctx->header_ready = true;
            header_notified = true;
            xSemaphoreGive(ctx->header_done);
            refill_when_low(ctx->file, input.get(), FLAC_INPUT_BUFFER_BYTES,
                            offset, length, eof);
        } else if (decode_result == FLAC_DECODER_SUCCESS) {
            if (!ctx->header_ready || samples_decoded == 0 ||
                samples_decoded % ctx->channels != 0) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            const size_t frames = samples_decoded / ctx->channels;
            result = ring_write(*ctx->ring, pcm.get(), frames, ctx->channels,
                                ctx->playback_allowed, ctx->callback_context);
            ctx->decoded_frames += frames;
            if (++decoded_blocks_since_yield >= 2) {
                decoded_blocks_since_yield = 0;
                vTaskDelay(1);
            }
            refill_when_low(ctx->file, input.get(), FLAC_INPUT_BUFFER_BYTES,
                            offset, length, eof);
        } else if (decode_result == FLAC_DECODER_NEED_MORE_DATA) {
            const bool received = refill_input(
                ctx->file, input.get(), FLAC_INPUT_BUFFER_BYTES, offset,
                length);
            if (!received) {
                eof = true;
                if (consumed == 0) {
                    ESP_LOGE(TAG, "truncated FLAC stream at end of file");
                    result = ESP_ERR_INVALID_RESPONSE;
                    break;
                }
            }
        } else if (decode_result == FLAC_DECODER_END_OF_STREAM) {
            break;
        } else {
            ESP_LOGE(TAG, "FLAC decode failed: %d",
                     static_cast<int>(decode_result));
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }

    if (result == ESP_OK && (!ctx->header_ready || ctx->decoded_frames == 0)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    ctx->result = result;
    if (!header_notified) {
        xSemaphoreGive(ctx->header_done);
    }
    ring_close(*ctx->ring);
    xSemaphoreGive(ctx->task_done);
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" esp_err_t flac_play_stream(
    FILE *file, flac_playback_allowed_cb_t playback_allowed,
    flac_volume_cb_t current_volume, flac_info_cb_t info_ready, void *context)
{
    if (file == nullptr || playback_allowed == nullptr ||
        current_volume == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    PcmRing ring;
    esp_err_t result = ring_init(ring, FLAC_PCM_RING_FRAMES);
    if (result != ESP_OK) {
        return result;
    }

    FlacDecodeContext decode_context;
    decode_context.file = file;
    decode_context.playback_allowed = playback_allowed;
    decode_context.callback_context = context;
    decode_context.ring = &ring;
    decode_context.header_done = xSemaphoreCreateBinary();
    decode_context.task_done = xSemaphoreCreateBinary();
    if (decode_context.header_done == nullptr ||
        decode_context.task_done == nullptr) {
        if (decode_context.header_done != nullptr) {
            vSemaphoreDelete(decode_context.header_done);
        }
        if (decode_context.task_done != nullptr) {
            vSemaphoreDelete(decode_context.task_done);
        }
        ring_destroy(ring);
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        flac_decode_task, "flac_decode", FLAC_TASK_STACK_BYTES,
        &decode_context, FLAC_TASK_PRIORITY, nullptr, 1);
    if (created != pdPASS) {
        vSemaphoreDelete(decode_context.header_done);
        vSemaphoreDelete(decode_context.task_done);
        ring_destroy(ring);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(decode_context.header_done, portMAX_DELAY);
    if (!decode_context.header_ready) {
        xSemaphoreTake(decode_context.task_done, portMAX_DELAY);
        result = decode_context.result;
        vSemaphoreDelete(decode_context.header_done);
        vSemaphoreDelete(decode_context.task_done);
        ring_destroy(ring);
        return result;
    }

    result = audio_output_set_sample_rate(decode_context.sample_rate);
    if (result != ESP_OK) {
        ring_cancel(ring);
    } else {
        if (info_ready != nullptr) {
            info_ready(decode_context.sample_rate, decode_context.channels,
                       decode_context.bits_per_sample, context);
        }
        ESP_LOGI(TAG,
                 "FLAC: %lu Hz, %lu channel(s), %lu-bit lossless, "
                 "max block=%lu, input cache=%lu KB, PCM ring=%lu KB",
                 static_cast<unsigned long>(decode_context.sample_rate),
                 static_cast<unsigned long>(decode_context.channels),
                 static_cast<unsigned long>(decode_context.bits_per_sample),
                 static_cast<unsigned long>(decode_context.max_block_size),
                 static_cast<unsigned long>(FLAC_INPUT_BUFFER_BYTES / 1024),
                 static_cast<unsigned long>(
                     FLAC_PCM_RING_FRAMES * 2 * sizeof(int32_t) / 1024));
    }

    while (result == ESP_OK &&
           ring_filled_frames(ring) < FLAC_PCM_PREFILL_FRAMES) {
        if (!playback_allowed(context)) {
            result = ESP_ERR_INVALID_STATE;
            ring_cancel(ring);
            break;
        }
        xSemaphoreTake(ring.mutex, portMAX_DELAY);
        const bool closed = ring.closed;
        xSemaphoreGive(ring.mutex);
        if (closed) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(FLAC_WAIT_MS));
    }

    static int32_t output[FLAC_OUTPUT_BLOCK_FRAMES * 2];
    while (result == ESP_OK) {
        if (!playback_allowed(context)) {
            result = ESP_ERR_INVALID_STATE;
            ring_cancel(ring);
            break;
        }
        size_t frames = 0;
        result = ring_read(ring, output, FLAC_OUTPUT_BLOCK_FRAMES, frames);
        if (result != ESP_OK || frames == 0) {
            break;
        }
        result = audio_output_write_q31(output, frames, 2,
                                        current_volume(context));
        if (result != ESP_OK) {
            ring_cancel(ring);
        }
    }

    xSemaphoreTake(decode_context.task_done, portMAX_DELAY);
    if (result == ESP_OK) {
        result = decode_context.result;
    }
    ESP_LOGI(TAG, "FLAC decoder processed %llu frame(s), underruns=%lu",
             decode_context.decoded_frames,
             static_cast<unsigned long>(ring.underruns));

    vSemaphoreDelete(decode_context.header_done);
    vSemaphoreDelete(decode_context.task_done);
    ring_destroy(ring);
    return result;
}
