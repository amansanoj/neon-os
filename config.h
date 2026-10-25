#pragma once
#include <Arduino.h>
namespace C {
constexpr char MAC[] = "be:16:c9:00:2f:53";
constexpr char NAME[] = "Neon Light";
constexpr uint8_t SDA_PIN=21, SCL_PIN=22, BUTTON_PIN=0, LED_PIN=2;
constexpr uint8_t OLED_ADDR=0x3C, OLED_CONTRAST=0x10;
constexpr bool LED_ACTIVE_HIGH=true;
constexpr uint32_t WRITE_GAP=70, RETRY_MIN=750, RETRY_MAX=30000;
constexpr uint32_t UI_REFRESH=400, DEBOUNCE=35, DOUBLE_MS=330, LONG_MS=2000;
constexpr uint32_t LED_PWM_FREQ=5000, LED_FADE_PERIOD=3000;
constexpr uint8_t LED_PWM_BITS=12;
constexpr uint16_t LED_MAX_DUTY=32;
}
