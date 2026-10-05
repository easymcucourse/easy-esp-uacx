/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_log.h"
#include "sdkconfig.h"

void easy_uacx_demo_run(void);

void app_main(void)
{
    ESP_LOGI("uac2_example_s3", "running UAC2 host unit tests");
    UNITY_BEGIN();
    unity_run_all_tests();
    if (UNITY_END()) { ESP_LOGE("uac2_example_s3", "unit tests failed; playback skipped"); return; }
#if CONFIG_EXAMPLE_PLAYBACK_TEST
    easy_uacx_demo_run();
#endif
}
