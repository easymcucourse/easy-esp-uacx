/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "uac2_internal.h"

/* read_pos / write_pos are monotonic counters; index = pos % size. */
esp_err_t audio_ring_init(audio_ring_t *r, size_t size)
{
    if (!r || !size) return ESP_ERR_INVALID_ARG;
    r->buffer = uac2_malloc(size);
    if (!r->buffer) return ESP_ERR_NO_MEM;
    r->size = size; r->read_pos = r->write_pos = 0;
    return ESP_OK;
}
void audio_ring_deinit(audio_ring_t *r)
{
    if (!r) return;
    uac2_free(r->buffer);
    memset(r, 0, sizeof(*r));
}
size_t audio_ring_used(const audio_ring_t *r) { return r->write_pos - r->read_pos; }
size_t audio_ring_free(const audio_ring_t *r) { return r->size - audio_ring_used(r); }

size_t audio_ring_write(audio_ring_t *r, const uint8_t *src, size_t len)
{
    size_t n = len < audio_ring_free(r) ? len : audio_ring_free(r);
    for (size_t i = 0; i < n; i++) r->buffer[(r->write_pos + i) % r->size] = src[i];
    r->write_pos += n;
    return n;
}
size_t audio_ring_read(audio_ring_t *r, uint8_t *dst, size_t len)
{
    size_t n = len < audio_ring_used(r) ? len : audio_ring_used(r);
    for (size_t i = 0; i < n; i++) dst[i] = r->buffer[(r->read_pos + i) % r->size];
    r->read_pos += n;
    return n;
}
