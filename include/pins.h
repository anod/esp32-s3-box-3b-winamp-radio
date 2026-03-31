// ============================================================
// ESP32-S3-BOX-3 — Pin Definitions
// ============================================================

#pragma once

// I2S Audio (ES8311 codec)
#define I2S_MCLK    2
#define I2S_BCLK    17
#define I2S_LRCK    45
#define I2S_DOUT    15

// I2C (shared bus: ES8311 codec + GT911 touch)
#define I2C_SDA     8
#define I2C_SCL     18

// Display — ILI9342C via SPI
#define LCD_SCK     7
#define LCD_MOSI    6
#define LCD_CS      5
#define LCD_DC      4
#define LCD_BL      47

// Touch — GT911 capacitive controller
#define TOUCH_INT   3

// Power amplifier enable
#define PA_PIN      46

// Buttons
#define BTN_BOOT    0

// ES8311 codec clock configuration
#define ES8311_SAMPLE_RATE      16000
#define ES8311_MCLK_MULTIPLE    256
#define ES8311_MCLK_FREQ_HZ    (ES8311_SAMPLE_RATE * ES8311_MCLK_MULTIPLE)
#define ES8311_VOICE_VOLUME     75
