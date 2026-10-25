
#pragma once

#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <cmath>

static bool displayEnabled = true;
static bool ledPwmReady = false;

static void indicator(uint32_t now) {
  if (!ledPwmReady) return;

  const bool active =
      WiFi.status() != WL_CONNECTED ||
      !bleConnected.load();

  uint32_t duty = 0;

  if (active) {
    const float phase =
        (now % C::LED_FADE_PERIOD) /
        static_cast<float>(C::LED_FADE_PERIOD);

    const float curve =
        0.5f - 0.5f * cosf(phase * 2.0f * PI);

    duty = static_cast<uint32_t>(
        lroundf(curve * C::LED_MAX_DUTY)
    );
  }

  constexpr uint32_t pwmMax =
      (1UL << C::LED_PWM_BITS) - 1UL;

  ledcWrite(
      C::LED_PIN,
      C::LED_ACTIVE_HIGH ? duty : pwmMax - duty
  );
}

static void drawTitle(int16_t left, int16_t top) {
  oled.setFont(&FreeSans9pt7b);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);

  int16_t x, y;
  uint16_t width, height;

  oled.getTextBounds(
      "NeonOS",
      0, 0,
      &x, &y,
      &width, &height
  );

  oled.setCursor(left - x, top - y);
  oled.print("NeonOS");
}

static void showSplash() {
  if (!oledReady || !displayEnabled) return;

  oled.clearDisplay();

  oled.setFont(&FreeSans9pt7b);
  oled.setTextSize(1);

  int16_t x, y;
  uint16_t width, height;

  oled.getTextBounds(
      "NeonOS",
      0, 0,
      &x, &y,
      &width, &height
  );

  const int16_t left =
      (128 - static_cast<int16_t>(width)) / 2;

  const int16_t top =
      (64 - static_cast<int16_t>(height)) / 2;

  drawTitle(left, top);
  oled.display();
}

static void drawMenuRow(
    uint8_t index,
    int16_t top,
    const char* label,
    const char* value
) {
  constexpr int16_t iconX = 7;
  constexpr int16_t labelX = 17;
  constexpr int16_t rightEdge = 126;

  oled.setFont(nullptr);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextWrap(false);

  const int16_t iconY = top + 3;

  if (index == selectedOption) {
    oled.fillCircle(
        iconX,
        iconY,
        3,
        SSD1306_WHITE
    );
  } else {
    oled.drawCircle(
        iconX,
        iconY,
        3,
        SSD1306_WHITE
    );
  }

  oled.setCursor(labelX, top);
  oled.print(label);

  int16_t x, y;
  uint16_t width, height;

  oled.getTextBounds(
      value,
      0, top,
      &x, &y,
      &width, &height
  );

  oled.setCursor(
      rightEdge - static_cast<int16_t>(width),
      top
  );

  oled.print(value);
}

static void render() {
  if (!oledReady || !displayEnabled) return;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  drawTitle(4, 1);

  oled.drawLine(
      2, 19, 125, 19,
      SSD1306_WHITE
  );

  char brightnessText[8];

  snprintf(
      brightnessText,
      sizeof(brightnessText),
      "%u%%",
      currentState.brightness
  );

  const int preset = currentPreset(currentState);

  const char* colorName =
      preset < 0
          ? "Custom"
          : COLORS[preset].name;

  drawMenuRow(
      0,
      25,
      "Power",
      currentState.on ? "On" : "Off"
  );

  drawMenuRow(
      1,
      38,
      "Brightness",
      brightnessText
  );

  drawMenuRow(
      2,
      51,
      "Color",
      colorName
  );

  oled.display();
}

static void toggleDisplay() {
  displayEnabled = !displayEnabled;

  if (storageReady) {
    preferences.putBool(
        "oled",
        displayEnabled
    );
  }

  if (oledReady) {
    oled.ssd1306_command(
        displayEnabled
            ? SSD1306_DISPLAYON
            : SSD1306_DISPLAYOFF
    );

    if (displayEnabled) {
      render();
    }
  }

  Serial.printf(
      "[OLED] %s\n",
      displayEnabled ? "ON" : "OFF"
  );
}

static void buttonTick(uint32_t now) {
  static bool rawPrev = true;
  static bool stable = true;
  static bool longConsumed = false;

  static uint32_t changedAt = 0;
  static uint32_t pressedAt = 0;
  static uint32_t releasedAt = 0;

  static uint8_t clickCount = 0;

  const bool raw =
      digitalRead(C::BUTTON_PIN) == HIGH;

  if (raw != rawPrev) {
    rawPrev = raw;
    changedAt = now;
  }

  if (
      raw != stable &&
      now - changedAt >= C::DEBOUNCE
  ) {
    stable = raw;

    if (!stable) {
      pressedAt = now;
      longConsumed = false;
    } else if (!longConsumed) {
      if (!displayEnabled) {
        clickCount = 0;
        return;
      }

      ++clickCount;
      releasedAt = now;

      if (clickCount == 2) {
        clickCount = 0;

        if (!light) return;

        if (selectedOption == 0) {
          light->setPowerLocal(
              !currentState.on
          );
        } else if (selectedOption == 1) {
          const uint8_t levels[] = {
              25, 50, 75, 100
          };

          uint8_t next = 25;

          for (uint8_t level : levels) {
            if (level > currentState.brightness) {
              next = level;
              break;
            }
          }

          light->setBrightnessLocal(next);
        } else {
          light->nextColorLocal();
        }
      }
    }
  }

  if (
      !stable &&
      !longConsumed &&
      now - pressedAt >= C::LONG_MS
  ) {
    longConsumed = true;
    clickCount = 0;
    toggleDisplay();
  }

  if (
      stable &&
      clickCount == 1 &&
      now - releasedAt > C::DOUBLE_MS
  ) {
    clickCount = 0;

    if (displayEnabled) {
      selectedOption =
          (selectedOption + 1) % 3;
    }
  }
}
