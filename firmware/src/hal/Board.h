#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

/**
 * @brief Hardware accessors and board-specific control helpers.
 */
namespace Board {
/// Initialize board peripherals.
bool init();

/// Service board-level background work.
void update();

/// Shared display driver instance.
Arduino_SH8601 &display();

/// Whether button A is currently pressed.
bool buttonAIsPressed();

/// Whether button B is currently pressed.
bool buttonBIsPressed();

/**
 * @brief Set display backlight brightness.
 * @param brightness Backlight level.
 */
void setDisplayBrightness(uint8_t brightness);

/// Current display brightness level.
uint8_t displayBrightness();

/**
 * @brief Enable or disable the speaker amplifier.
 * @param enabled True to enable the amplifier.
 */
void setAudioAmpEnabled(bool enabled);

/// Battery charge estimate as a percentage.
int batteryLevel();

/// Battery voltage in millivolts.
uint16_t batteryVoltageMv();

/// USB VBUS voltage in millivolts.
uint16_t vbusVoltageMv();

/// Whether external USB power is present.
bool usbConnected();

/// Human-readable label for the current power source.
const char *powerSourceLabel();

/// Power down the board.
void powerOff();
} // namespace Board
