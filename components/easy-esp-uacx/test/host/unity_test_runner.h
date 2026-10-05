/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "unity.h"

void host_register_test(UnityTestFunction function, const char *name, int line);
#define HOST_JOIN_(a, b) a##b
#define HOST_JOIN(a, b) HOST_JOIN_(a, b)
#define TEST_CASE(name, tags) \
    static void HOST_JOIN(host_test_, __LINE__)(void); \
    static void __attribute__((constructor)) HOST_JOIN(host_register_, __LINE__)(void) \
    { host_register_test(HOST_JOIN(host_test_, __LINE__), name, __LINE__); } \
    static void HOST_JOIN(host_test_, __LINE__)(void)
