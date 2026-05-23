#pragma once

#include <Arduino.h>
#include <functional>

/**
 * @brief Power state enumeration for device power management
 */
enum class PowerState {
  /// Device is fully active, screen on, WiFi connected.
  Active,

  /// Screen brightness reduced but still visible.
  Dimmed,

  /// Screen is off while WiFi remains connected.
  ScreenOff,

  /// Transitional state while waking back to active mode.
  Waking,

  /// Low-power state with WiFi disconnected.
  LightSleep,

  /// Device is fully powered down.
  PowerOff,
};

/**
 * @brief Get human-readable name of a power state
 * @param state Power state to get name for
 * @return String representation of the power state
 */
const char *powerStateName(PowerState state);

/**
 * @brief Timeout configuration for power state transitions
 */
struct PowerTimeouts {
  /// Milliseconds before dimming the screen.
  unsigned long dimMs;

  /// Milliseconds before turning the screen off.
  unsigned long screenOffMs;

  /// Milliseconds before entering light sleep.
  unsigned long lightSleepMs;

  /// Milliseconds before powering the device off.
  unsigned long powerOffMs;
};

/**
 * @brief Manages device power state transitions and idle timeout handling
 */
class PowerManager {
public:
  PowerManager();

  /**
   * @brief Update power state based on elapsed idle time
   * Should be called regularly in the main loop
   */
  void update();

  /**
   * @brief Register user activity to reset idle timer
   */
  void registerActivity();

  /**
   * @brief Set timeout thresholds for power state transitions
   * @param timeouts Structure containing timeout values for each state
   */
  void setTimeouts(const PowerTimeouts &timeouts);

  /**
   * @brief Get the current timeout configuration
   * @return Reference to the PowerTimeouts structure
   */
  const PowerTimeouts &timeouts() const { return _timeouts; }

  /**
   * @brief Get the current power state
   * @return Current PowerState
   */
  PowerState getState() const { return _state; }

  /**
   * @brief Get the elapsed idle time in milliseconds
   * @return Milliseconds since last activity
   */
  unsigned long getIdleTime() const;

  /**
   * @brief Check if current state can be interrupted by user activity
   * @return true if in Dimmed, ScreenOff, or LightSleep state
   */
  bool isInterruptible() const {
    return _state == PowerState::Dimmed || _state == PowerState::ScreenOff ||
           _state == PowerState::LightSleep;
  }

  /**
   * @brief Check if device is currently waking up
   * @return true if in Waking state
   */
  bool isWaking() const { return _state == PowerState::Waking; }

  /**
   * @brief Begin the wake-up process from low power state
   */
  void beginWaking();

  /**
   * @brief Complete the wake-up process
   */
  void finishWaking();

  /**
   * @brief Restore device to fully active state
   */
  void restoreActive();

  /**
   * @brief Set the brightness level to save before entering low power state
   * @param brightness Brightness value to save
   */
  void setSavedBrightness(int brightness) { _savedBrightness = brightness; }

  /**
   * @brief Register callback for brightness changes
   * @param callback Function to call with brightness level (int)
   */
  void onBrightnessChange(std::function<void(int)> callback) {
    _brightnessCallback = callback;
  }

  /**
   * @brief Register callback for WiFi state changes
   * @param callback Function to call with WiFi enable/disable flag (bool)
   */
  void onWiFiStateChange(std::function<void(bool)> callback) {
    _wifiCallback = callback;
  }

  /**
   * @brief Register callback for power off event
   * @param callback Function to call when device should power down
   */
  void onPowerOff(std::function<void()> callback) {
    _powerOffCallback = callback;
  }

private:
  /// Current power state of the device
  PowerState _state;

  /// Timestamp (in millis) of the last user activity
  unsigned long _lastActivityMs;

  /// Brightness level saved before entering dim/screen-off state
  int _savedBrightness;

  /// Timeout thresholds for each power state transition
  PowerTimeouts _timeouts;

  /// Callback invoked when brightness should change (int = brightness level)
  std::function<void(int)> _brightnessCallback;

  /// Callback invoked when WiFi should be toggled (bool = should enable)
  std::function<void(bool)> _wifiCallback;

  /// Callback invoked when device should power down completely
  std::function<void()> _powerOffCallback;

  /**
   * @brief Transition to a new power state
   * @param newState Target power state
   */
  void transitionTo(PowerState newState);

  /**
   * @brief Enter light sleep mode
   */
  void enterLightSleep();
};
