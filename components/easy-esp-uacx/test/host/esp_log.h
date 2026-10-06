/* SPDX-FileCopyrightText: 2026 easymcucourse
 * SPDX-License-Identifier: MIT */
#pragma once
#include <stdio.h>
#define ESP_LOGW(tag, ...) do { (void)(tag); printf(__VA_ARGS__); printf("\n"); } while (0)
