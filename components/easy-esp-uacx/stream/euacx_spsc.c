/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#include <string.h>
#include "euacx_model.h"
uint32_t euacx_ring_used(const euacx_ring_t *r)
{
    uint32_t read = atomic_load_explicit(&r->read, memory_order_acquire);
    return atomic_load_explicit(&r->write, memory_order_acquire) - read;
}
uint32_t euacx_ring_space(const euacx_ring_t *r) { return r->size - euacx_ring_used(r); }
size_t euacx_ring_put(euacx_ring_t *r, const void *data, size_t size)
{
    uint32_t w = atomic_load_explicit(&r->write, memory_order_relaxed);
    uint32_t free = r->size - (w - atomic_load_explicit(&r->read, memory_order_acquire));
    if (size > free) size = free;
    uint32_t at = w & (r->size - 1);
    size_t first = size < r->size - at ? size : r->size - at;
    memcpy(r->data + at, data, first); memcpy(r->data, (const uint8_t *)data + first, size - first);
    atomic_store_explicit(&r->write, w + size, memory_order_release);
    return size;
}
size_t euacx_ring_take(euacx_ring_t *r, void *data, size_t size)
{
    uint32_t read = atomic_load_explicit(&r->read, memory_order_relaxed);
    uint32_t used = atomic_load_explicit(&r->write, memory_order_acquire) - read;
    if (size > used) size = used;
    uint32_t at = read & (r->size - 1);
    size_t first = size < r->size - at ? size : r->size - at;
    memcpy(data, r->data + at, first); memcpy((uint8_t *)data + first, r->data, size - first);
    atomic_store_explicit(&r->read, read + size, memory_order_release);
    return size;
}
