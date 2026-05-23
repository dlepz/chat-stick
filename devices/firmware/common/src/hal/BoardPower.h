#pragma once

#include "hal/DeviceCapabilities.h"
#include <Arduino.h>

namespace Board {
const DeviceCapabilities &capabilities();
void setAudioAmpEnabled(bool enabled);
bool usbConnected();
LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs);
} // namespace Board
