#pragma once

#include <stddef.h>
#include <stdint.h>

namespace th2config {
constexpr uint8_t DHT_PIN = 14;
constexpr uint8_t ADC_PIN = 34;
constexpr uint8_t BUTTON_PIN = 27;
constexpr uint8_t SDA_PIN = 21;
constexpr uint8_t SCL_PIN = 22;

constexpr uint32_t SAMPLE_PERIOD_MS = 3000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr size_t MAX_SAMPLES = 300;

// Verify the actual LCD geometry before uploading. I2C address/pin mapping
// are detected by hd44780_I2Cexp; this is a default, not hardware evidence.
constexpr bool LCD_ENABLED = true;
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;

}  // namespace th2config

static_assert(th2config::SAMPLE_PERIOD_MS >= 2000,
              "Adafruit DHT reads must be at least 2 seconds apart");
static_assert(th2config::LCD_COLS >= 8 && th2config::LCD_COLS <= 40,
              "Supported LCD width is 8..40");
static_assert(th2config::LCD_ROWS >= 1 && th2config::LCD_ROWS <= 4,
              "Supported LCD height is 1..4");
