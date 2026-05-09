#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

namespace Board {
bool init();
void update();

Arduino_SH8601 &display();

bool buttonAIsPressed();
bool buttonBIsPressed();

void setDisplayBrightness(uint8_t brightness);
uint8_t displayBrightness();
void setAudioAmpEnabled(bool enabled);

int batteryLevel();
uint16_t batteryVoltageMv();
uint16_t vbusVoltageMv();
bool usbConnected();
const char *powerSourceLabel();

void powerOff();
} // namespace Board
