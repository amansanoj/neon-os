#include <Arduino.h>
#include "config.h"
#include "colors.h"
static LightState currentState{false,100,{255,255,255},0,0};
static int submissions=0;
static void submit(const LightState&) { ++submissions; }
#include "homekit-light.h"
#include "interface.h"

static uint32_t clockNow=100;
static void advance(uint32_t ms) {
  for(uint32_t i=0;i<ms;++i) buttonTick(++clockNow);
}
static void press() { buttonHigh=false; advance(C::DEBOUNCE+1); }
static void release() { buttonHigh=true; advance(C::DEBOUNCE+1); }
static void clicks(int count) {
  for(int i=0;i<count;++i) {
    press(); advance(40); release();
    if(i+1<count) advance(70);
  }
}
static void settle() { advance(C::DOUBLE_MS+1); }

int main(int argc,char** argv) {
  assert(argc==2);
  NeonLight device;
  light=&device;
  submissions=0;
  const std::string scenario=argv[1];
  if(scenario=="colors") {
    const RGB expected[]={{255,170,84},{255,224,196},{204,219,255},
                          {255,0,0},{0,255,0},{0,0,255},{128,0,128}};
    for(unsigned round=0;round<3;++round) {
      for(unsigned i=0;i<7;++i) {
        device.nextColorLocal();
        assert(sameRgb(currentState.rgb,expected[i]));
        assert(currentPreset(currentState)==static_cast<int>(i));
        RGB restored{};
        assert(restorePresetRgb(currentState.hue,currentState.saturation,restored));
        assert(sameRgb(restored,expected[i]));
      }
    }
    assert(submissions==21);
  } else if(scenario=="single") {
    for(int i=0;i<3;++i) { clicks(1); settle(); assert(selectedOption==(i+1)%3); }
    assert(submissions==0);
  } else if(scenario=="double") {
    clicks(2); assert(submissions==0); settle();
    assert(currentState.on && submissions==1 && selectedOption==0);
    selectedOption=1;
    for(int value: {25,50,75,100,25}) {
      clicks(2); settle(); assert(currentState.brightness==value);
    }
    selectedOption=2;
    for(int i=0;i<15;++i) {
      clicks(2); settle(); assert(currentPreset(currentState)==i%7);
    }
  } else if(scenario=="four") {
    for(int option=0;option<3;++option) {
      selectedOption=option;
      clicks(4); assert(!displayFlipped); settle();
      assert(displayFlipped && oled.rotation==2 && selectedOption==option);
      assert(preferences.getBool("flipped",false));
      clicks(4); settle(); assert(!displayFlipped && oled.rotation==0);
    }
    assert(submissions==0);
  } else if(scenario=="invalid") {
    for(int n: {3,5,6,20,260}) { clicks(n); settle(); }
    assert(!displayFlipped && selectedOption==0 && submissions==0);
  } else if(scenario=="hold") {
    clicks(2); advance(30); press(); advance(C::LONG_MS);
    assert(!displayEnabled && submissions==0);
    release(); settle();
    press(); advance(C::LONG_MS); release(); settle();
    assert(displayEnabled && selectedOption==0 && submissions==0);
  } else if(scenario=="off") {
    displayEnabled=false;
    clicks(1); settle(); clicks(2); settle(); clicks(4); settle();
    assert(!displayEnabled && !displayFlipped && submissions==0 && selectedOption==0);
  } else if(scenario=="bounce") {
    for(int i=0;i<12;++i) { buttonHigh=!buttonHigh; advance(5); }
    settle(); assert(selectedOption==0 && submissions==0);
    clicks(2); settle(); assert(submissions==1);
  } else if(scenario=="boundary") {
    clicks(1);
    // Start the second press just before the gap expires; debouncing
    // crosses the deadline, but the gesture must remain a double click.
    advance(C::DOUBLE_MS-10); press(); release(); settle();
    assert(submissions==1 && selectedOption==0);
  } else if(scenario=="late") {
    clicks(1); settle(); clicks(1); settle();
    assert(selectedOption==2 && submissions==0);
  } else if(scenario=="wrap") {
    clockNow=UINT32_MAX-200;
    clicks(4); settle(); assert(displayFlipped && submissions==0);
    clicks(2); settle(); assert(submissions==1);
  } else if(scenario=="no-storage") {
    storageReady=false; oledReady=false;
    clicks(4); settle(); assert(displayFlipped && preferences.values.empty());
  } else { assert(false); }
}
