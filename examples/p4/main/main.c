/*
 * SPDX-FileCopyrightText: 2026 easymcucourse
 *
 * SPDX-License-Identifier: MIT
 */
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_log.h"
#include "sdkconfig.h"

/*
 * Example entry point.
 *
 * 1. Runs the easy_uacx unit tests (Unity) first; these need no hardware.
 * 2. If they pass and CONFIG_EXAMPLE_PLAYBACK_TEST is enabled
 *    (idf.py menuconfig -> "UAC2 example"), starts the playback demo in
 *    ../../common/easy_uacx_demo.c. Plug a USB DAC into the USB OTG port and
 *    watch the log for "RESULT ... status=PASS" lines.
 *
 * To build your own application, start from easy_uacx_demo.c: it shows
 * euacx_init(), the connect/stop callbacks, and pull/push streaming.
 */

/* Defined in examples/common/easy_uacx_demo.c. Never returns. */
void easy_uacx_demo_run(void);

void app_main(void)
{
    ESP_LOGI("uac2_example_p4", "running UAC2 host unit tests");
    UNITY_BEGIN();
    unity_run_all_tests();
    if (UNITY_END()) { ESP_LOGE("uac2_example_p4", "unit tests failed; playback skipped"); return; }
#if CONFIG_EXAMPLE_PLAYBACK_TEST
    easy_uacx_demo_run();
#endif
}
