
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HomeSpan.h>
#include <SpanRollback.h>
#include <esp_heap_caps.h>
#include "ota-updater.h"
#include "firmware-version.h"
#include <atomic>
#include <cmath>
#include <string>

#include "config.h"

static Preferences preferences;
static bool storageReady = false;

#include "colors.h"

static QueueHandle_t stateQueue = nullptr;

static std::atomic<uint32_t> bleHeartbeat{0};
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

#include "persisted-state.h"
static TaskHandle_t bleTaskHandle=nullptr;

static void submit(const LightState& state) {
  stateWrites.changed(millis());
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

  Serial.println("\nNeonOS " NEON_FIRMWARE_VERSION);
  Serial.println(NeonOta::identity());
  // Watch the main control task throughout setup and normal polling.
  homeSpan.enableWatchdog(20);
  homeSpan.setCompileTime();
  homeSpan.setSketchVersion(NEON_FIRMWARE_VERSION);

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

  restoreLightState();

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
  Wire.setTimeOut(20);

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

  bleHeartbeat.store(millis());
  if (
      xTaskCreatePinnedToCore(
          bleWorker,
          "neon-ble",
          8192,
          nullptr,
          1,
          &bleTaskHandle,
          0
      ) != pdPASS
  ) {
    Serial.println(
        "[FATAL] BLE worker"
    );
    abort();
  }
  if(!NeonOta::begin()) {
    Serial.println("[OTA] Initialization failed");
    abort();
  }
}

void loop() {
  homeSpan.poll();
  const uint32_t now=millis();
  buttonTick(now);
  static uint32_t lastIndicator=0,lastRender=0,lastDiagnostics=0;
  if(Runtime::elapsed(now,lastIndicator,20)) { indicator(now); lastIndicator=now; }
  if(Runtime::elapsed(now,lastRender,C::UI_REFRESH)) { render(); lastRender=now; }
  persistLightState(now);
  const bool workerHealthy=!Runtime::elapsed(now,bleHeartbeat.load(),C::TASK_STALL_MS);
  if(!workerHealthy) {
    Serial.println("[HEALTH] BLE worker stalled; restarting"); ESP.restart();
  }
  NeonOta::tick(now,storageReady && stateQueue && light && workerHealthy);
  if(NeonOta::rebootRequested()) {
    persistLightState(now,true);
    Serial.println("[OTA] Verified release ready; rebooting");
    ESP.restart();
  }
  if(Runtime::elapsed(now,lastDiagnostics,C::DIAGNOSTICS_MS)) {
    lastDiagnostics=now;
    Serial.printf("[HEALTH] heap=%u min=%u largest=%u stacks main=%u BLE=%u OTA=%u\n",
        ESP.getFreeHeap(),ESP.getMinFreeHeap(),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(bleTaskHandle)),
        static_cast<unsigned>(NeonOta::stackHeadroom()));
  }
}
