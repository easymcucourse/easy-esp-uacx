#pragma once

#include "driver/gpio.h"
#include "driver/spi_common.h"

/* User-confirmed SPI microSD/module wiring. */
#define MUSIC_SD_SPI_HOST       SPI2_HOST
#define MUSIC_SD_PIN_CS         GPIO_NUM_15
#define MUSIC_SD_PIN_MOSI       GPIO_NUM_16
#define MUSIC_SD_PIN_CLK        GPIO_NUM_17
#define MUSIC_SD_PIN_MISO       GPIO_NUM_18
#define MUSIC_SD_RUN_FREQ_KHZ   20000

/* Native 240x320 ST7789, rotated left 90 degrees to logical 320x240.
 * Keep it on SPI3 so SDSPI can own SPI2 independently. */
#define MUSIC_LCD_SPI_HOST       SPI3_HOST
#define MUSIC_LCD_PIN_SCLK       GPIO_NUM_42
#define MUSIC_LCD_PIN_MOSI       GPIO_NUM_41
#define MUSIC_LCD_PIN_RST        GPIO_NUM_40
#define MUSIC_LCD_PIN_DC         GPIO_NUM_39
#define MUSIC_LCD_PIN_CS         GPIO_NUM_38
#define MUSIC_LCD_WIDTH          320
#define MUSIC_LCD_HEIGHT         240
#define MUSIC_LCD_PIXEL_FREQ_HZ  (40 * 1000 * 1000)

/* Generic Philips-I2S DAC/sound-card output. DIN is driven by ESP32 DOUT. */
#define MUSIC_I2S_PIN_WS         GPIO_NUM_9
#define MUSIC_I2S_PIN_DOUT       GPIO_NUM_10
#define MUSIC_I2S_PIN_BCLK       GPIO_NUM_11
#define MUSIC_I2S_PIN_MCLK       GPIO_NUM_12
#define MUSIC_I2S_PIN_SD         GPIO_NUM_13
#define MUSIC_I2S_SD_ENABLE_LEVEL 1

/* External audio/USB power sequencing requested by PCB bring-up. */
#define MUSIC_POWER_INPUT_GPIO       GPIO_NUM_3
#define MUSIC_AUDIO_POWER_GPIO       GPIO_NUM_47
#define MUSIC_AUDIO_POWER_PULSE_MS   100
#define MUSIC_AUDIO_POWER_SETTLE_MS  10000

/* Button nets are still unknown; UART controls remain available. */
#define MUSIC_BUTTON_PLAY_PAUSE GPIO_NUM_NC
#define MUSIC_BUTTON_NEXT       GPIO_NUM_NC
#define MUSIC_BUTTON_PREVIOUS   GPIO_NUM_NC
#define MUSIC_BUTTON_VOL_UP     GPIO_NUM_NC
#define MUSIC_BUTTON_VOL_DOWN   GPIO_NUM_NC

#define MUSIC_ENABLE_BOOT_TONE  0
