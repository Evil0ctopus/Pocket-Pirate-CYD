#pragma once

#include <Arduino.h>

namespace CheapBlackDisplay {

// Hosyond ESP32-S3 2.8-inch ILI9341 / FT6336G module.
constexpr int TFT_CS = 10;
constexpr int TFT_DC = 46;
constexpr int TFT_SCK = 12;
constexpr int TFT_MOSI = 11;
constexpr int TFT_MISO = 13;
constexpr int TFT_BL = 45;

constexpr int TOUCH_SDA = 16;
constexpr int TOUCH_SCL = 15;
constexpr int TOUCH_RST = 18;
constexpr int TOUCH_INT = 17;
constexpr uint8_t TOUCH_ADDRESS = 0x38;

constexpr int SD_CLK = 38;
constexpr int SD_CMD = 40;
constexpr int SD_D0 = 39;
constexpr int SD_D1 = 41;
constexpr int SD_D2 = 48;
constexpr int SD_D3 = 47;

constexpr int AUDIO_ENABLE = 1;
constexpr int I2S_MCK = 4;
constexpr int I2S_SCK = 5;
constexpr int I2S_SDO = 6;
constexpr int I2S_LRC = 7;
constexpr int I2S_SDI = 8;

constexpr int BATTERY_ADC = 9;
constexpr int RGB_LED = 42;

}  // namespace CheapBlackDisplay
