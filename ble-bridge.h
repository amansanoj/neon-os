#pragma once
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <atomic>
#include <string>
#include "runtime-policy.h"

static std::atomic<int> bleDisconnectReason{0};
static bool sendFrame(const uint8_t (&frame)[9], const char* what) {
  if(!bleClient || !bleClient->isConnected() || !bleCharacteristic)return false;
  const bool ok=bleCharacteristic->writeValue(frame,sizeof(frame),writeResponse);
  if(!ok)Serial.printf("[BLE] %s write failed\n",what);
  return ok;
}
static bool sendPower(bool on) {
  const uint8_t f[9]={0x7E,0x04,0x04,static_cast<uint8_t>(on?0xF0:0),0,
      static_cast<uint8_t>(on?1:0),0xFF,0,0xEF};
  return sendFrame(f,"power");
}
static bool sendColor(const LightState& s) {
  const uint8_t f[9]={0x7E,0x07,0x05,0x03,s.rgb.r,s.rgb.g,s.rgb.b,0x10,0xEF};
  return sendFrame(f,"color");
}
static bool sendBrightness(uint8_t value) {
  const uint8_t f[9]={0x7E,0x04,0x01,static_cast<uint8_t>(std::min<int>(value,100)),
      0x01,0xFF,0xFF,0,0xEF};
  return sendFrame(f,"brightness");
}
class BleEvents : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*,int reason) override {
    // Host callback never blocks on UART, flash or another BLE operation.
    bleDisconnectReason.store(reason);
    bleDropped.store(true); bleConnected.store(false);
  }
};
static BleEvents events;
static void logBleLink() {
  if(!bleClient->isConnected())return;
  const int rssi=bleClient->getRssi();
  if(rssi)Serial.printf("[BLE] RSSI=%d dBm\n",rssi);
  else Serial.println("[BLE] RSSI unavailable");
}
static bool connectBle(uint8_t type, bool refresh=true) {
  bleCharacteristic=nullptr;
  if(bleClient->isConnected())bleClient->disconnect();
  const NimBLEAddress address(std::string(C::MAC),type);
  if(!bleClient->connect(address,refresh,false,false)) {
    const int error=bleClient->getLastError();
    Serial.printf("[BLE] Connect type=%u failed: %d (%s)\n",type,error,
        NimBLEUtils::returnCodeToString(error));
    return false;
  }
  auto* svc=bleClient->getService(NimBLEUUID(static_cast<uint16_t>(0xFFF0)));
  if(svc)bleCharacteristic=svc->getCharacteristic(NimBLEUUID(static_cast<uint16_t>(0xFFF3)));
  if(!bleCharacteristic || (!bleCharacteristic->canWrite() && !bleCharacteristic->canWriteNoResponse())) {
    bleCharacteristic=nullptr; bleClient->disconnect();
    Serial.println("[BLE] No writable FFF0/FFF3"); return false;
  }
  writeResponse=!bleCharacteristic->canWriteNoResponse();
  bleConnected.store(true);
  const auto info=bleClient->getConnInfo();
  Serial.printf("[BLE] Connected type=%u interval=%.2f ms timeout=%u ms\n",
      type,info.getConnInterval()*1.25,static_cast<unsigned>(info.getConnTimeout())*10);
  logBleLink();
  return true;
}
static void bleWorker(void*) {
  LightState desired{false,100,{255,255,255},0,0},applied{};
  bool linked=false,powerSynced=false,colorSynced=false,dimSynced=false;
  bool known=false,refresh=true;
  uint8_t addrType=BLE_ADDR_PUBLIC;
  uint32_t retry=C::RETRY_MIN,lastAttempt=millis(),retryDelay=0;
  uint32_t lastWrite=millis()-C::WRITE_GAP,lastLinkLog=millis();
  for(;;) {
    bleHeartbeat.store(millis());
    LightState incoming;
    if(xQueueReceive(stateQueue,&incoming,0)==pdTRUE)desired=incoming;
    const uint32_t now=millis();
    const bool dropped=bleDropped.exchange(false);
    if(linked && (dropped || !bleClient->isConnected())) {
      const int reason=bleDisconnectReason.load();
      Serial.printf("[BLE] Link lost: %d (%s)\n",reason,NimBLEUtils::returnCodeToString(reason));
      linked=false; bleConnected.store(false); bleCharacteristic=nullptr;
      powerSynced=colorSynced=dimSynced=false;
      lastAttempt=now; retryDelay=C::RETRY_MIN;
    }
    if(!linked && Runtime::elapsed(now,lastAttempt,retryDelay)) {
      linked=connectBle(addrType,refresh);
      lastAttempt=millis();
      if(linked) {
        known=true; refresh=false; retry=C::RETRY_MIN;
        powerSynced=colorSynced=dimSynced=false;
        lastWrite=millis()-C::WRITE_GAP; lastLinkLog=millis();
      } else {
        bleConnected.store(false); refresh=true;
        if(!known)addrType=addrType==BLE_ADDR_PUBLIC?BLE_ADDR_RANDOM:BLE_ADDR_PUBLIC;
        retryDelay=retry; retry=std::min<uint32_t>(retry*2,C::RETRY_MAX);
      }
      continue;
    }
    if(linked && Runtime::elapsed(now,lastLinkLog,C::BLE_LINK_LOG_MS)) {
      logBleLink(); lastLinkLog=millis();
    }
    if(linked && Runtime::elapsed(now,lastWrite,C::WRITE_GAP)) {
      bool attempt=false,ok=true;
      if(!powerSynced || applied.on!=desired.on) {
        attempt=true; ok=sendPower(desired.on);
        if(ok) { applied.on=desired.on; powerSynced=true;
          if(desired.on)colorSynced=dimSynced=false; }
      } else if(desired.on && (!colorSynced || !sameRgb(applied.rgb,desired.rgb))) {
        attempt=true; ok=sendColor(desired);
        if(ok) { applied.rgb=desired.rgb; colorSynced=true; dimSynced=false; }
      } else if(desired.on && (!dimSynced || applied.brightness!=desired.brightness)) {
        attempt=true; ok=sendBrightness(desired.brightness);
        if(ok) { applied.brightness=desired.brightness; dimSynced=true; }
      }
      // Refresh the timestamp even when idle: an ancient deadline must not
      // suppress writes after 2^31 ms (~25 days) of unchanged light state.
      lastWrite=millis();
      if(attempt && !ok) {
        bleCharacteristic=nullptr; bleClient->disconnect(); linked=false;
        bleConnected.store(false); refresh=true;
        powerSynced=colorSynced=dimSynced=false;
        lastAttempt=millis(); retryDelay=C::RETRY_MIN;
      }
    }
    // A single-slot queue coalesces slider bursts. Wake promptly for changes,
    // but yield even when commands arrive continuously or the light is idle.
    if(xQueueReceive(stateQueue,&incoming,pdMS_TO_TICKS(C::WRITE_GAP))==pdTRUE)
      desired=incoming;
    vTaskDelay(1);
  }
}
