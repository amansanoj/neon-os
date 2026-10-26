#pragma once
#include <Arduino.h>
#include <cmath>
struct RGB { uint8_t r, g, b; };
struct LightState { bool on; uint8_t brightness; RGB rgb; float hue, saturation; };
static RGB rgbFor(float h,float sat){
 h=fmodf(h,360.f);if(h<0)h+=360.f;
 float s=constrain(sat,0.f,100.f)/100.f;
 float c=s,x=c*(1.f-fabsf(fmodf(h/60.f,2.f)-1.f)),m=1.f-c;
 float r=0,g=0,b=0;
 if(h<60){r=c;g=x;}else if(h<120){r=x;g=c;}
 else if(h<180){g=c;b=x;}else if(h<240){g=x;b=c;}
 else if(h<300){r=x;b=c;}else{r=c;b=x;}
 return {(uint8_t)lroundf((r+m)*255.f),(uint8_t)lroundf((g+m)*255.f),(uint8_t)lroundf((b+m)*255.f)};
}


struct ColorPreset { const char* name; RGB rgb; };
static constexpr ColorPreset COLORS[] = {
 {"Warm",    {255,170, 84}},
 {"Neutral", {255,224,196}},
 {"Cool",    {204,219,255}},
 {"Red",     {255,  0,  0}},
 {"Green",   {  0,255,  0}},
 {"Blue",    {  0,  0,255}},
 {"Purple",  {128,  0,128}}
};
static constexpr uint8_t COLOR_COUNT = sizeof(COLORS)/sizeof(COLORS[0]);
static bool sameRgb(const RGB& a,const RGB& b) {
 return a.r==b.r && a.g==b.g && a.b==b.b;
}
static int currentPreset(const LightState& s) {
 for(uint8_t i=0;i<COLOR_COUNT;i++)
  if(sameRgb(s.rgb,COLORS[i].rgb)) return i;
 return -1;
}
static void rgbToHs(const RGB& c,float& h,float& s) {
 const float r=c.r/255.f,g=c.g/255.f,b=c.b/255.f;
 const float high=fmaxf(r,fmaxf(g,b)),low=fminf(r,fminf(g,b));
 const float d=high-low;
 s=high<=0.f?0.f:(d/high)*100.f;
 if(d<0.00001f) {h=0.f;return;}
 if(high==r) h=60.f*fmodf((g-b)/d,6.f);
 else if(high==g) h=60.f*((b-r)/d+2.f);
 else h=60.f*((r-g)/d+4.f);
 if(h<0.f)h+=360.f;
}
static float hueGap(float a,float b) {
 float d=fabsf(a-b);
 return fminf(d,360.f-d);
}
static bool restorePresetRgb(float hue,float sat,RGB& out) {
 if (!storageReady) return false;
 uint8_t index=preferences.getUChar("preset",255);
 if(index>=COLOR_COUNT) return false;
 float ph,ps;
 rgbToHs(COLORS[index].rgb,ph,ps);
 if(hueGap(hue,ph)>0.5f || fabsf(sat-ps)>0.5f) return false;
 out=COLORS[index].rgb;
 return true;
}
