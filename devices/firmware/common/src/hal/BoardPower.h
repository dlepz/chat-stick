#pragma once

#include "hal/DeviceCapabilities.h"
#include <Arduino.h>

/**
 * @brief Board-specific power and capability hooks used by shared firmware code.
 */
namespace Board {
/**
 * @brief Return the capability flags for the active board implementation.
 * @return Immutable board capability descriptor.
 */
const DeviceCapabilities &capabilities();

/**
 * @brief Enable or disable the board audio amplifier when one is controllable.
 * @param enabled True to enable the amplifier.
 */
void setAudioAmpEnabled(bool enabled);

/**
 * @brief Report whether USB power is currently present.
 * @return True when USB/VBUS power is detected.
 */
bool usbConnected();

/**
 * @brief Enter one light-sleep interval and report the wake reason.
 * @param wakeIntervalMs Timer wake interval in milliseconds.
 * @return Reason the device left light sleep.
 */
LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs);

/**
 * @brief Report why this boot resumed from deep sleep.
 * @return Deep sleep wake reason from the board/platform.
 */
DeepSleepWakeReason deepSleepWakeReason();

/**
 * @brief Enter deep sleep until the timer or a wake-capable button fires.
 * @param sleepUs Timer wake interval in microseconds.
 */
void enterDeepSleep(uint64_t sleepUs);
} // namespace Board
