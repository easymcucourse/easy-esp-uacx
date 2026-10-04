#include "unity.h"
#include "unity_test_runner.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_LOGI("uac2_example_p4", "running UAC2 host unit tests");
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
