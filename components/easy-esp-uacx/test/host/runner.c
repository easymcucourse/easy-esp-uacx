/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "unity_test_runner.h"

static struct { UnityTestFunction function; const char *name; int line; } tests[64];
static size_t num_tests, allocated;
/* max_align_t preserves malloc's alignment after our accounting prefix. */
typedef union { max_align_t alignment; size_t size; } allocation_t;

void host_register_test(UnityTestFunction function, const char *name, int line)
{
    if (num_tests == sizeof(tests) / sizeof(tests[0])) abort();
    tests[num_tests].function = function;
    tests[num_tests].name = name;
    tests[num_tests++].line = line;
}

void *uac2_malloc(size_t size)
{
    allocation_t *p = malloc(sizeof(*p) + size);
    if (!p) return NULL;
    p->size = size;
    allocated += size;
    return p + 1;
}

void uac2_free(void *ptr)
{
    if (!ptr) return;
    allocation_t *p = (allocation_t *)ptr - 1;
    allocated -= p->size;
    free(p);
}

size_t esp_get_free_heap_size(void) { return 1024u * 1024u - allocated; }
void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UnityBegin("test_easy_uacx.c");
    for (size_t i = 0; i < num_tests; ++i) {
        UnityDefaultTestRun(tests[i].function, tests[i].name, tests[i].line);
    }
    int failed = UnityEnd();
    if (allocated) {
        fprintf(stderr, "host allocations leaked: %zu bytes\n", allocated);
        return 1;
    }
    return failed;
}
