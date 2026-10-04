#include "unity.h"
#include "unity_test_runner.h"
#include "esp_log.h"
#include "sdkconfig.h"

void uac2_playback_demo_run(void);

void app_main(void)
{
    ESP_LOGI("uac2_example_p4", "running UAC2 host unit tests");
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
#if CONFIG_EXAMPLE_PLAYBACK_TEST
    uac2_playback_demo_run();
#endif
}
