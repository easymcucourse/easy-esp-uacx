#include "i2s_output.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"

#define I2S_DEFAULT_SAMPLE_RATE 48000U
#define I2S_WRITE_TIMEOUT_MS    2000U

static const char *TAG = "i2s_output";
static i2s_chan_handle_t s_tx_channel;
static uint32_t s_sample_rate;
static bool s_channel_enabled;

static i2s_std_clk_config_t make_clock_config(uint32_t sample_rate)
{
    i2s_std_clk_config_t config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return config;
}

esp_err_t i2s_output_init(void)
{
    if (s_tx_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(gpio_reset_pin(MUSIC_I2S_PIN_SD), TAG,
                        "reset SD/enable pin");
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(MUSIC_I2S_PIN_SD, GPIO_MODE_OUTPUT), TAG,
        "configure SD/enable pin");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(MUSIC_I2S_PIN_SD, !MUSIC_I2S_SD_ENABLE_LEVEL), TAG,
        "hold sound card in shutdown");

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 8;
    channel_config.dma_frame_num = 256;
    channel_config.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(
        i2s_new_channel(&channel_config, &s_tx_channel, NULL), TAG,
        "allocate I2S TX channel");

    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = MUSIC_I2S_PIN_MCLK,
            .bclk = MUSIC_I2S_PIN_BCLK,
            .ws = MUSIC_I2S_PIN_WS,
            .dout = MUSIC_I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    esp_err_t error =
        i2s_channel_init_std_mode(s_tx_channel, &standard_config);
    if (error != ESP_OK) {
        (void)i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        return error;
    }

    error = i2s_channel_enable(s_tx_channel);
    if (error != ESP_OK) {
        (void)i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        return error;
    }
    s_channel_enabled = true;
    s_sample_rate = I2S_DEFAULT_SAMPLE_RATE;

    ESP_RETURN_ON_ERROR(
        gpio_set_level(MUSIC_I2S_PIN_SD, MUSIC_I2S_SD_ENABLE_LEVEL), TAG,
        "enable sound card");
    ESP_LOGI(TAG,
             "I2S READY: Philips stereo 32-bit, WS=%d DIN=%d BCK=%d "
             "MCLK=%d (256fs) SD=%d level=%d",
             MUSIC_I2S_PIN_WS, MUSIC_I2S_PIN_DOUT, MUSIC_I2S_PIN_BCLK,
             MUSIC_I2S_PIN_MCLK, MUSIC_I2S_PIN_SD,
             MUSIC_I2S_SD_ENABLE_LEVEL);
    return ESP_OK;
}

esp_err_t i2s_output_set_sample_rate(uint32_t sample_rate)
{
    if (s_tx_channel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate == s_sample_rate) {
        return ESP_OK;
    }

    if (s_channel_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_channel), TAG,
                            "disable channel for clock change");
        s_channel_enabled = false;
    }

    const i2s_std_clk_config_t clock_config =
        make_clock_config(sample_rate);
    ESP_RETURN_ON_ERROR(
        i2s_channel_reconfig_std_clock(s_tx_channel, &clock_config), TAG,
        "change I2S sample rate");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_channel), TAG,
                        "enable I2S channel");
    s_channel_enabled = true;
    s_sample_rate = sample_rate;
    ESP_LOGI(TAG, "I2S stream: %lu Hz, 32-bit stereo, MCLK=%lu Hz",
             (unsigned long)sample_rate,
             (unsigned long)(sample_rate * 256U));
    return ESP_OK;
}

esp_err_t i2s_output_write(const int32_t *stereo_pcm, size_t frame_count)
{
    if (s_tx_channel == NULL || !s_channel_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stereo_pcm == NULL || frame_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t byte_count = frame_count * 2U * sizeof(*stereo_pcm);
    size_t bytes_written = 0;
    ESP_RETURN_ON_ERROR(
        i2s_channel_write(s_tx_channel, stereo_pcm, byte_count,
                          &bytes_written, I2S_WRITE_TIMEOUT_MS), TAG,
        "I2S DMA write");
    return bytes_written == byte_count ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
