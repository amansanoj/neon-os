#pragma once
#include <cstring>
#include "runtime-policy.h"

// NVS provides CRC-protected, atomic blob replacement. Explicit fields keep the
// on-flash representation independent of C++ struct padding and bool layout.
// Float fields use the ESP32's 32-bit little-endian IEEE-754 representation.
static Runtime::DeferredWrite stateWrites;
static LightState lastSavedState{};
static bool haveSavedState = false;
static bool validState(const LightState& s) {
  return s.brightness <= 100 && std::isfinite(s.hue) &&
      std::isfinite(s.saturation) && s.hue >= 0 && s.hue <= 360 &&
      s.saturation >= 0 && s.saturation <= 100;
}
static bool equalState(const LightState& a, const LightState& b) {
  return a.on == b.on && a.brightness == b.brightness && sameRgb(a.rgb,b.rgb) &&
      a.hue == b.hue && a.saturation == b.saturation;
}
static void encodeState(const LightState& s, uint8_t (&out)[14]) {
  out[0]=1; out[1]=s.on; out[2]=s.brightness;
  out[3]=s.rgb.r; out[4]=s.rgb.g; out[5]=s.rgb.b;
  static_assert(sizeof(float)==4, "NVS format requires 32-bit float");
  memcpy(out+6,&s.hue,4); memcpy(out+10,&s.saturation,4);
}
static bool decodeState(const uint8_t (&in)[14], LightState& s) {
  if(in[0]!=1 || in[1]>1)return false;
  LightState candidate{in[1]!=0,in[2],{in[3],in[4],in[5]},0,0};
  memcpy(&candidate.hue,in+6,4); memcpy(&candidate.saturation,in+10,4);
  if(!validState(candidate))return false;
  s=candidate; return true;
}
static void restoreLightState() {
  if(!storageReady)return;
  uint8_t bytes[14]{};
  if(preferences.getBytesLength("state-v1")==sizeof(bytes) &&
      preferences.getBytes("state-v1",bytes,sizeof(bytes))==sizeof(bytes) &&
      decodeState(bytes,currentState)) {
    lastSavedState=currentState; haveSavedState=true; return;
  }
  if(preferences.getBytesLength("state-v1"))
    Serial.println("[NVS] Invalid state record; loading legacy/default values");
  // Read old HomeSpan 2.1.8 CHAR keys once, without enabling characteristic
  // auto-persistence. Preserve the original accessory/IID schema for pairing.
  Preferences legacy;
  if(legacy.begin("CHAR",true)) {
    currentState.on=legacy.getULong64("002500000001007",0)!=0;
    currentState.brightness=static_cast<uint8_t>(
        std::min<uint64_t>(legacy.getULong64("000800000001008",100),100));
    uint64_t h=legacy.getULong64("002300000001009",0);
    uint64_t sat=legacy.getULong64("002F0000000100A",0);
    double hd=0,sd=0;
    memcpy(&hd,&h,sizeof(h)); memcpy(&sd,&sat,sizeof(sat));
    currentState.hue=static_cast<float>(hd);
    currentState.saturation=static_cast<float>(sd);
    legacy.end();
    if(!validState(currentState))currentState={false,100,{255,255,255},0,0};
    currentState.rgb=rgbFor(currentState.hue,currentState.saturation);
    restorePresetRgb(currentState.hue,currentState.saturation,currentState.rgb);
  }
  stateWrites.changed(millis());
}
static bool persistLightState(uint32_t now, bool force=false) {
  if(!storageReady)return false;
  if(haveSavedState && equalState(lastSavedState,currentState))return true;
  if(!force && !stateWrites.due(now))return false;
  uint8_t bytes[14]; encodeState(currentState,bytes);
  if(preferences.putBytes("state-v1",bytes,sizeof(bytes))!=sizeof(bytes)) {
    stateWrites.retryLater(now);
    Serial.println("[NVS] State save failed; will retry");
    return false;
  }
  lastSavedState=currentState; haveSavedState=true; stateWrites.committed(now);
  return true;
}
