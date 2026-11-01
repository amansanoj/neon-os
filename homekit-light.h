#pragma once
#include <HomeSpan.h>

// Persistent state belongs to the coalesced application record, rather than a
// flash commit for every HomeKit slider sample and characteristic notification.
struct NeonLight : Service::LightBulb {
  Characteristic::On power{currentState.on,false};
  Characteristic::Brightness brightness{currentState.brightness,false};
  Characteristic::Hue hue{currentState.hue,false};
  Characteristic::Saturation saturation{currentState.saturation,false};

  NeonLight() : Service::LightBulb() { submit(currentState); }

  boolean update() override {
    LightState next=currentState;
    next.on=power.getNewVal<bool>();
    const int b=brightness.getNewVal<int>();
    const float h=hue.getNewVal<float>(), s=saturation.getNewVal<float>();
    if(b<0 || b>100 || !std::isfinite(h) || !std::isfinite(s) ||
       h<0 || h>360 || s<0 || s>100)return false;
    next.brightness=static_cast<uint8_t>(b);
    const bool colorChanged=(hue.updated() || saturation.updated()) &&
        (hueGap(next.hue,h)>0.01f || fabsf(next.saturation-s)>0.01f);
    if(colorChanged) { next.hue=h; next.saturation=s; next.rgb=rgbFor(h,s); }
    currentState=next;
    submit(currentState);
    return true;
  }
  void setPowerLocal(bool on) {
    power.setVal(on); currentState.on=on; submit(currentState);
  }
  void setBrightnessLocal(uint8_t b) {
    if(b>100)return;
    brightness.setVal(static_cast<int>(b));
    currentState.brightness=b; submit(currentState);
  }
  void nextColorLocal() {
    const int current=currentPreset(currentState);
    const uint8_t next=current<0?0:static_cast<uint8_t>((current+1)%COLOR_COUNT);
    float h=0,s=0; rgbToHs(COLORS[next].rgb,h,s);
    hue.setVal(h); saturation.setVal(s);
    currentState.hue=h; currentState.saturation=s;
    currentState.rgb=COLORS[next].rgb;
    submit(currentState);
  }
};
static NeonLight* light=nullptr;
