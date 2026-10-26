
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HomeSpan.h>
#include <atomic>
#include <cmath>
#include <string>

#include "config.h"

static Preferences preferences;
static bool storageReady = false;

#include "colors.h"

static QueueHandle_t stateQueue = nullptr;

static std::atomic<bool> bleConnected{false};
static std::atomic<bool> bleDropped{false};

static LightState currentState{
    false,
    100,
    {255, 255, 255},
    0,
    0
};

static Adafruit_SSD1306 oled(
    128,
    64,
    &Wire,
    -1
);

static bool oledReady = false;
static uint8_t selectedOption = 0;

static NimBLEClient* bleClient = nullptr;

static NimBLERemoteCharacteristic*
    bleCharacteristic = nullptr;

static bool writeResponse = false;

static bool due(
    uint32_t now,
    uint32_t deadline
) {
  return static_cast<int32_t>(
      now - deadline
  ) >= 0;
}

static void submit(const LightState& state) {
  if (stateQueue) {
    xQueueOverwrite(
        stateQueue,
        &state
    );
  }
}

#include "ble-bridge.h"
#include "homekit-light.h"
#include "interface.h"

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\nNeonOS");

  pinMode(
      C::BUTTON_PIN,
      INPUT_PULLUP
  );

  pinMode(
      C::LED_PIN,
      OUTPUT
  );

  digitalWrite(
      C::LED_PIN,
      C::LED_ACTIVE_HIGH ? LOW : HIGH
  );

  ledPwmReady = ledcAttach(
      C::LED_PIN,
      C::LED_PWM_FREQ,
      C::LED_PWM_BITS
  );

  if (ledPwmReady) {
    constexpr uint32_t maxDuty =
        (1UL << C::LED_PWM_BITS) - 1UL;

    ledcWrite(
        C::LED_PIN,
        C::LED_ACTIVE_HIGH ? 0 : maxDuty
    );
  } else {
    Serial.println(
        "[LED] PWM unavailable"
    );
  }

  storageReady = preferences.begin(
      "neon-color",
      false
  );

  if (!storageReady) {
    Serial.println(
        "[NVS] Storage unavailable"
    );
  }

  displayEnabled = storageReady
      ? preferences.getBool("oled", true)
      : true;

  displayFlipped = storageReady
      ? preferences.getBool("flipped", false)
      : false;

  Wire.begin(
      C::SDA_PIN,
      C::SCL_PIN
  );

  Wire.setClock(400000);

  oledReady = oled.begin(
      SSD1306_SWITCHCAPVCC,
      C::OLED_ADDR
  );

  if (oledReady) {
    oled.setRotation(displayFlipped ? 2 : 0);
    oled.ssd1306_command(
        SSD1306_SETCONTRAST
    );

    oled.ssd1306_command(
        C::OLED_CONTRAST
    );

    if (displayEnabled) {
      showSplash();
    } else {
      oled.clearDisplay();
      oled.display();

      oled.ssd1306_command(
          SSD1306_DISPLAYOFF
      );
    }
  } else {
    Serial.println(
        "[OLED] Display unavailable"
    );
  }

  stateQueue = xQueueCreate(
      1,
      sizeof(LightState)
  );

  if (!stateQueue) {
    Serial.println(
        "[FATAL] State queue"
    );
    abort();
  }

  NimBLEDevice::init("");
  const int bleTxPower = NimBLEDevice::getPower();
  if (bleTxPower != 0xFF) {
    Serial.printf("[BLE] TX power=%d dBm\n", bleTxPower);
  } else {
    Serial.println("[BLE] TX power unavailable");
  }

  bleClient = NimBLEDevice::createClient();

  if (!bleClient) {
    Serial.println(
        "[FATAL] BLE client"
    );
    abort();
  }

  bleClient->setClientCallbacks(
      &events,
      false
  );

  bleClient->setConnectTimeout(5000);

  homeSpan.begin(
      Category::Lighting,
      C::NAME
  );

  new SpanAccessory();

  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(C::NAME);
  new Characteristic::Manufacturer("DIY / MELK");
  new Characteristic::Model(
      "ESP32 BLE HomeKit Bridge"
  );

  light = new NeonLight();

  if (
      xTaskCreatePinnedToCore(
          bleWorker,
          "neon-ble",
          8192,
          nullptr,
          1,
          nullptr,
          0
      ) != pdPASS
  ) {
    Serial.println(
        "[FATAL] BLE worker"
    );
    abort();
  }
}

void loop() {
  homeSpan.poll();

  const uint32_t now = millis();

  buttonTick(now);
  indicator(now);

  static uint32_t nextRender = 0;

  if (due(now, nextRender)) {
    nextRender = now + C::UI_REFRESH;
    render();
  }
}
