/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stddef.h>

/* Host-only allocation accounting; this does not emulate the ESP-IDF heap. */
size_t esp_get_free_heap_size(void);
