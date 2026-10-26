
#pragma once

#include <Wire.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSans9pt7b.h>
#include <cmath>

static bool displayEnabled = true;
static bool displayFlipped = false;
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

  char rgbText[12];  // "255,255,255" plus the terminator.
  if (preset < 0) {
    snprintf(
        rgbText,
        sizeof(rgbText),
        "%u,%u,%u",
        static_cast<unsigned>(currentState.rgb.r),
        static_cast<unsigned>(currentState.rgb.g),
        static_cast<unsigned>(currentState.rgb.b)
    );
  }

  const char* colorName =
      preset < 0
          ? rgbText
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

static void flipDisplay() {
  displayFlipped = !displayFlipped;

  if (storageReady &&
      preferences.putBool("flipped", displayFlipped) != 1) {
    Serial.println("[NVS] Could not save display orientation");
  }

  if (oledReady) {
    oled.setRotation(displayFlipped ? 2 : 0);
    render();
  }

  Serial.printf("[OLED] Rotation %u degrees\n",
                displayFlipped ? 180 : 0);
}

static void handleClicks(uint8_t count) {
  if (!displayEnabled) return;

  if (count == 1) {
    selectedOption = (selectedOption + 1) % 3;
  } else if (count == 4) {
    flipDisplay();
  } else if (count == 2 && light) {
    if (selectedOption == 0) {
      light->setPowerLocal(!currentState.on);
    } else if (selectedOption == 1) {
      const uint8_t levels[] = {25, 50, 75, 100};
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
  // Three clicks and sequences longer than four have no action.
}

static void buttonTick(uint32_t now) {
  static bool rawPrev = true;
  static bool stable = true;
  static bool longConsumed = false;
  static uint32_t changedAt = 0;
  static uint32_t pressedAt = 0;
  static uint32_t releasedAt = 0;
  static uint8_t clickCount = 0;

  const bool raw = digitalRead(C::BUTTON_PIN) == HIGH;

  if (raw != rawPrev) {
    rawPrev = raw;
    changedAt = now;
  }

  // Resolve only after the inter-click window closes. A press that starts
  // inside the window belongs to this sequence, even while it debounces.
  if (stable && clickCount != 0 &&
      now - releasedAt > C::DOUBLE_MS &&
      (raw || changedAt - releasedAt > C::DOUBLE_MS)) {
    const uint8_t completed = clickCount;
    clickCount = 0;
    handleClicks(completed);
  }

  if (raw != stable && now - changedAt >= C::DEBOUNCE) {
    stable = raw;

    if (!stable) {
      pressedAt = now;
      longConsumed = false;
    } else if (!longConsumed && displayEnabled) {
      // Saturate at an invalid count so a long burst cannot wrap into
      // a valid gesture or accidentally toggle the display repeatedly.
      if (clickCount < 5) ++clickCount;
      releasedAt = now;
    } else {
      clickCount = 0;
    }
  }

  if (!stable && !longConsumed &&
      now - pressedAt >= C::LONG_MS) {
    longConsumed = true;
    clickCount = 0;
    toggleDisplay();
  }
}
