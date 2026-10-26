#!/usr/bin/env python3
"""Exercise actual firmware headers with host-only hardware/API substitutes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOCKS = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <cassert>
using boolean = bool;
constexpr int HIGH=1, WL_CONNECTED=1;
constexpr int SSD1306_WHITE=1, SSD1306_DISPLAYON=1, SSD1306_DISPLAYOFF=0;
constexpr float PI=3.14159265358979323846f;
template<class T> T constrain(T x,T a,T b) { return std::max(a,std::min(x,b)); }
static bool buttonHigh=true;
int digitalRead(int) { return buttonHigh; }
void ledcWrite(int,uint32_t) {}
struct { int status() { return WL_CONNECTED; } } WiFi;
struct {
 template<class... T> void printf(const char*,T...) {}
 void println(const char*) {}
} Serial;
struct {
 std::map<std::string,int> values;
 bool getBool(const char* k,bool d) { return values.count(k)?values[k]!=0:d; }
 size_t putBool(const char* k,bool v) { values[k]=v; return 1; }
 uint8_t getUChar(const char* k,uint8_t d) { return values.count(k)?values[k]:d; }
 void putUChar(const char* k,uint8_t v) { values[k]=v; }
 bool isKey(const char* k) { return values.count(k); }
 void remove(const char* k) { values.erase(k); }
} preferences;
static bool storageReady=true;
namespace Service {
 struct LightBulb { virtual bool update() { return true; } };
}
namespace Characteristic {
 struct Value {
  double value;
  Value(double v,bool):value(v) {}
  template<class T> T getVal() { return static_cast<T>(value); }
  template<class T> T getNewVal() { return static_cast<T>(value); }
  template<class T> void setVal(T v) { value=v; }
  bool updated() { return false; }
 };
 using On=Value; using Brightness=Value; using Hue=Value; using Saturation=Value;
}
static int FreeSans9pt7b;
struct {
 int rotation=0, frames=0;
 int cursorX=0, printedX=0;
 std::string printedText;
 void setRotation(int r) { rotation=r; }
 void setFont(const int*) {}
 void setTextSize(int) {}
 void setTextColor(int) {}
 void setTextWrap(bool) {}
 void getTextBounds(const char* text,int,int,int16_t* x,int16_t* y,uint16_t* w,uint16_t* h) {
  *x=*y=0; *w=std::string(text).size()*6; *h=8;
 }
 void setCursor(int x,int) { cursorX=x; }
 void print(const char* text) { printedText=text; printedX=cursorX; }
 void clearDisplay() {}
 void display() { ++frames; }
 void fillCircle(int,int,int,int) {}
 void drawCircle(int,int,int,int) {}
 void drawLine(int,int,int,int,int) {}
 void ssd1306_command(int) {}
} oled;
static bool oledReady=true;
static uint8_t selectedOption=0;
static std::atomic<bool> bleConnected{true};
'''
with tempfile.TemporaryDirectory(prefix='neon-tests-') as temp:
    tmp = Path(temp)
    (tmp / 'Arduino.h').write_text(MOCKS)
    for header in ['Wire.h', 'WiFi.h', 'Adafruit_GFX.h', 'Adafruit_SSD1306.h',
                   'HomeSpan.h', 'Fonts/FreeSans9pt7b.h']:
        path = tmp / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
    binary = tmp / 'regression'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-fsanitize=undefined',
                    '-I', str(tmp), '-I', str(ROOT),
                    str(ROOT / 'tests/regression.cpp'), '-o', str(binary)], check=True)
    for scenario in ['colors', 'single', 'double', 'four', 'invalid', 'hold',
                     'off', 'bounce', 'boundary', 'late', 'wrap', 'no-storage', 'rgb-display']:
        subprocess.run([str(binary), scenario], check=True)
        print(f'PASS {scenario}', flush=True)
