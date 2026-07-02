#pragma once

#include <Arduino.h>
#include <functional>

/**
 * @brief Ordered idle-power states, from fully active to powered off.
 */
enum class PowerState {
  /// Normal interactive state.
  Active,

  /// Display is dimmed but the device remains responsive.
  Dimmed,

  /// Display is off and low-power settings may be applied.
  ScreenOff,

  /// Device is restoring services after an interruptible low-power state.
  Waking,

  /// Board light sleep is active between wake checks.
  LightSleep,

  /// Device is handing off to board power-off/deep-sleep behavior.
  PowerOff,
};

/**
 * @brief Human-readable name for a power state.
 * @param state Power state to describe.
 * @return Static state name string.
 */
const char *powerStateName(PowerState state);

/**
 * @brief Idle timeout thresholds for the power-state cascade.
 */
struct PowerTimeouts {
  /// Idle duration before dimming the display.
  unsigned long dimMs;

  /// Idle duration before turning the screen off.
  unsigned long screenOffMs;

  /// Idle duration before entering light sleep.
  unsigned long lightSleepMs;

  /// Idle duration before powering off.
  unsigned long powerOffMs;
};

/**
 * @brief Manages idle-power transitions and related hardware callbacks.
 */
class PowerManager {
public:
  /// Callback signature for mirrored power log lines.
  using LogCallback = std::function<void(char side, const char *topic,
                                         const char *message)>;

  /// Construct a manager with configured default timeout values.
  PowerManager();

  /// Advance idle-state transitions based on elapsed inactivity.
  void update();

  /// Mark user or network activity and restore from interruptible states.
  void registerActivity();

  /**
   * @brief Replace timeout thresholds, clamping them into a valid order.
   * @param timeouts Requested timeout thresholds.
   */
  void setTimeouts(const PowerTimeouts &timeouts);

  /// Current normalized timeout thresholds.
  const PowerTimeouts &timeouts() const { return _timeouts; }

  /// Current power state.
  PowerState getState() const { return _state; }

  /// Milliseconds since the most recent registered activity.
  unsigned long getIdleTime() const;

  /// Whether the current low-power state can be interrupted by activity.
  bool isInterruptible() const {
    return _state == PowerState::Dimmed || _state == PowerState::ScreenOff ||
           _state == PowerState::LightSleep;
  }

  /// Whether the device is currently restoring from a low-power state.
  bool isWaking() const { return _state == PowerState::Waking; }

  /// Begin a wake transition from an interruptible state.
  void beginWaking();

  /// Complete a wake transition and return to active state.
  void finishWaking();

  /// Restore active power settings immediately.
  void restoreActive();

  /**
   * @brief Cache the brightness value used when restoring from dim/off states.
   * @param brightness Brightness level to restore.
   */
  void setSavedBrightness(int brightness) { _savedBrightness = brightness; }

  /**
   * @brief Register a callback for display brightness changes.
   * @param callback Callback receiving the new brightness level.
   */
  void onBrightnessChange(std::function<void(int)> callback) {
    _brightnessCallback = callback;
  }

  /**
   * @brief Register a callback for WiFi enable/disable transitions.
   * @param callback Callback receiving true when WiFi should be enabled.
   */
  void onWiFiStateChange(std::function<void(bool)> callback) {
    _wifiCallback = callback;
  }

  /**
   * @brief Register a callback for CPU frequency changes.
   * @param callback Callback receiving the target frequency in MHz.
   */
  void onCpuFrequencyChange(std::function<void(int)> callback) {
    _cpuFrequencyCallback = callback;
  }

  /**
   * @brief Register a callback for final power-off handling.
   * @param callback Callback invoked when the power-off state is reached.
   */
  void onPowerOff(std::function<void()> callback) {
    _powerOffCallback = callback;
  }

  /**
   * @brief Register a callback for power-manager log lines.
   * @param callback Log sink callback.
   */
  void onLog(LogCallback callback) { _logCallback = callback; }

private:
  /// Current power state.
  PowerState _state;

  /// Timestamp of the most recent registered activity.
  unsigned long _lastActivityMs;

  /// Brightness restored when leaving dim/off states.
  int _savedBrightness;

  /// Current normalized idle timeout thresholds.
  PowerTimeouts _timeouts;

  /// Callback used to apply display brightness changes.
  std::function<void(int)> _brightnessCallback;

  /// Callback used to enable or disable WiFi.
  std::function<void(bool)> _wifiCallback;

  /// Callback used to apply CPU frequency changes.
  std::function<void(int)> _cpuFrequencyCallback;

  /// Callback used to perform final power-off work.
  std::function<void()> _powerOffCallback;

  /// Optional mirrored log sink.
  LogCallback _logCallback;

  /// Whether idle power-off is allowed in the current power-source state.
  bool canIdlePowerOff() const;

  /// Apply a CPU frequency change through the registered callback.
  void applyCpuFrequency(int mhz);

  /// Move to a new power state and apply side effects.
  void transitionTo(PowerState newState);

  /// Enter board light sleep until button wake or power-off timeout.
  void enterLightSleep();

  /// Emit a client-side power log line.
  void logClient(const char *fmt, ...) const;

  /// Emit a server-side power log line.
  void logServer(const char *fmt, ...) const;
};
