#include <Arduino.h>
#include "colors.h"
static uint32_t testClock=0;
uint32_t millis() { return testClock; }
static LightState currentState{false,100,{255,255,255},0,0};
#include "persisted-state.h"
int main(int argc,char** argv) {
 assert(argc==2);
 const std::string scenario=argv[1];
 if(scenario=="roundtrip") {
  const LightState original{true,37,{23,54,87},210.5f,73.25f};
  uint8_t data[14]; encodeState(original,data);
  LightState restored{}; assert(decodeState(data,restored));
  assert(equalState(restored,original));
  preferences.putBytes("state-v1",data,sizeof(data));
  restoreLightState(); assert(equalState(currentState,original));
  assert(Preferences::writes==1);
  assert(persistLightState(10000)); assert(Preferences::writes==1);
 } else if(scenario=="corrupt") {
  uint8_t data[14]{}; encodeState(currentState,data);
  data[0]=2; assert(!decodeState(data,currentState));
  encodeState(currentState,data); data[1]=2; assert(!decodeState(data,currentState));
  encodeState(currentState,data); data[2]=101; assert(!decodeState(data,currentState));
  encodeState(currentState,data); const float invalid=NAN;
  memcpy(data+6,&invalid,4); assert(!decodeState(data,currentState));
 } else if(scenario=="legacy") {
  Preferences::legacy["002500000001007"]=1;
  Preferences::legacy["000800000001008"]=65;
  const double h=120,s=100;
  memcpy(&Preferences::legacy["002300000001009"],&h,8);
  memcpy(&Preferences::legacy["002F0000000100A"],&s,8);
  preferences.putUChar("preset",4);
  restoreLightState();
  assert(currentState.on && currentState.brightness==65);
  assert(sameRgb(currentState.rgb,RGB{0,255,0}));
  assert(!persistLightState(4999)); assert(persistLightState(5000));
  assert(preferences.getBytesLength("state-v1")==14);
  assert(Preferences::legacy["000800000001008"]==65); // Old namespace unchanged.
 } else if(scenario=="failed-write") {
  restoreLightState(); Preferences::failWrites=true;
  assert(!persistLightState(5000)); assert(Preferences::writes==0);
  Preferences::failWrites=false;
  assert(!persistLightState(9999)); assert(persistLightState(10000));
  assert(Preferences::writes==1);
 } else if(scenario=="coalescing") {
  restoreLightState();
  for(uint32_t t=0;t<10000;t+=10) {
   currentState.brightness=(t/10)%100; stateWrites.changed(t);
   assert(!persistLightState(t));
  }
  assert(persistLightState(15000)); assert(Preferences::writes==1);
  currentState.brightness=77; stateWrites.changed(16000);
  assert(!persistLightState(74999)); assert(persistLightState(75000));
  assert(Preferences::writes==2);
  assert(persistLightState(100000)); assert(Preferences::writes==2);
  currentState.on=true; stateWrites.changed(100001);
  assert(persistLightState(100001,true)); assert(Preferences::writes==3);
 } else assert(false);
}
