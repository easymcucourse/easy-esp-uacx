#include "buttons.h"

#include <stdbool.h>
#include <stddef.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music_player.h"

#define BUTTON_POLL_MS     10
#define BUTTON_DEBOUNCE_MS 40

typedef void (*button_action_t)(void);

typedef struct {
    gpio_num_t pin;
    const char *name;
    button_action_t action;
    int last_level;
    TickType_t changed_at;
    bool pressed_reported;
} button_t;

static const char *TAG = "buttons";
static button_t s_buttons[] = {
    {MUSIC_BUTTON_PLAY_PAUSE, "play/pause", music_player_toggle_pause, 1, 0,
     false},
    {MUSIC_BUTTON_NEXT, "next", music_player_next, 1, 0, false},
    {MUSIC_BUTTON_PREVIOUS, "previous", music_player_previous, 1, 0, false},
    {MUSIC_BUTTON_VOL_UP, "volume up", music_player_volume_up, 1, 0, false},
    {MUSIC_BUTTON_VOL_DOWN, "volume down", music_player_volume_down, 1, 0,
     false},
};

static void button_task(void *argument)
{
    (void)argument;
    const TickType_t debounce = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]);
             ++index) {
            button_t *button = &s_buttons[index];
            if (button->pin == GPIO_NUM_NC) {
                continue;
            }
            const int level = gpio_get_level(button->pin);
            if (level != button->last_level) {
                button->last_level = level;
                button->changed_at = now;
                button->pressed_reported = false;
            }
            if (level == 0 && !button->pressed_reported &&
                now - button->changed_at >= debounce) {
                button->pressed_reported = true;
                ESP_LOGI(TAG, "%s", button->name);
                button->action();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

esp_err_t buttons_init(void)
{
    bool any_enabled = false;
    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]);
         ++index) {
        button_t *button = &s_buttons[index];
        if (button->pin == GPIO_NUM_NC) {
            continue;
        }
        any_enabled = true;
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << button->pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&config));
        button->last_level = gpio_get_level(button->pin);
        button->changed_at = xTaskGetTickCount();
    }

    if (!any_enabled) {
        ESP_LOGW(TAG,
                 "button GPIOs are disabled; use UART0 controls or update "
                 "board_pins.h from the PCB netlist");
        return ESP_OK;
    }

    const BaseType_t created =
        xTaskCreate(button_task, "buttons", 3072, NULL, 4, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
