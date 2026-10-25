#pragma once
#include <HomeSpan.h>
struct NeonLight : Service::LightBulb {
 Characteristic::On power{false,true};
 Characteristic::Brightness brightness{100,true};
 Characteristic::Hue hue{0,true};
 Characteristic::Saturation saturation{0,true};
 NeonLight():Service::LightBulb(){
    const float h=hue.getVal<float>(),s=saturation.getVal<float>();
  RGB rgb=rgbFor(h,s);
  restorePresetRgb(h,s,rgb);
  currentState={power.getVal<bool>(),(uint8_t)brightness.getVal<int>(),rgb,h,s};
  submit(currentState);
 }
 boolean update() override {
  const float newHue=hue.getNewVal<float>();
  const float newSat=saturation.getNewVal<float>();
      const bool colorChanged=(hue.updated() || saturation.updated()) &&
      (hueGap(currentState.hue,newHue)>0.01f ||
       fabsf(currentState.saturation-newSat)>0.01f);
  currentState.on=power.getNewVal<bool>();
  currentState.brightness=(uint8_t)brightness.getNewVal<int>();
  if(colorChanged) {
   currentState.hue=newHue;
   currentState.saturation=newSat;
   currentState.rgb=rgbFor(newHue,newSat);
   if(storageReady && preferences.isKey("preset")) preferences.remove("preset");
  }
  submit(currentState);
  Serial.printf("[HOME] on=%d brightness=%u RGB=%u,%u,%u\n",
   currentState.on,currentState.brightness,currentState.rgb.r,currentState.rgb.g,currentState.rgb.b);
  return true;
 }
 void setPowerLocal(bool on){
  power.setVal(on);currentState.on=on;submit(currentState);
  Serial.printf("[BUTTON] Power %s\n",on?"ON":"OFF");
 }
 void setBrightnessLocal(uint8_t b){
  brightness.setVal((int)b);currentState.brightness=b;submit(currentState);
  Serial.printf("[BUTTON] Brightness %u%%\n",b);
 }
 void nextColorLocal(){
  const int current=currentPreset(currentState);
  const uint8_t next=(current<0)?0:(uint8_t)((current+1)%COLOR_COUNT);
    const RGB chosen=COLORS[next].rgb;
  float newHue=0.f, newSat=0.f;
  rgbToHs(chosen,newHue,newSat);
  Serial.printf("[PRESET] Selected %s: %u,%u,%u, HS=%.1f/%.1f\n",
      COLORS[next].name,chosen.r,chosen.g,chosen.b,newHue,newSat);

      hue.setVal(newHue);
  saturation.setVal(newSat);

      currentState.hue=newHue;
  currentState.saturation=newSat;
  currentState.rgb=chosen;
  submit(currentState);
  if(storageReady) preferences.putUChar("preset",next);
  Serial.printf("[PRESET] Committed %s: %u,%u,%u\n",
      COLORS[next].name,currentState.rgb.r,currentState.rgb.g,currentState.rgb.b);
 }

};
static NeonLight *light=nullptr;
