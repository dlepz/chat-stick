#pragma once

#include <Arduino.h>
#include <functional>

enum class PowerState {
  Active,
  Dimmed,
  ScreenOff,
  Waking,
  LightSleep,
  PowerOff,
};

const char *powerStateName(PowerState state);

struct PowerTimeouts {
  unsigned long dimMs;
  unsigned long screenOffMs;
  unsigned long lightSleepMs;
  unsigned long powerOffMs;
};

class PowerManager {
public:
  using LogCallback = std::function<void(char side, const char *topic,
                                         const char *message)>;

  PowerManager();

  void update();
  void registerActivity();
  void setTimeouts(const PowerTimeouts &timeouts);
  const PowerTimeouts &timeouts() const { return _timeouts; }

  PowerState getState() const { return _state; }
  unsigned long getIdleTime() const;

  bool isInterruptible() const {
    return _state == PowerState::Dimmed || _state == PowerState::ScreenOff ||
           _state == PowerState::LightSleep;
  }

  bool isWaking() const { return _state == PowerState::Waking; }

  void beginWaking();
  void finishWaking();
  void restoreActive();

  void setSavedBrightness(int brightness) { _savedBrightness = brightness; }

  void onBrightnessChange(std::function<void(int)> callback) {
    _brightnessCallback = callback;
  }

  void onWiFiStateChange(std::function<void(bool)> callback) {
    _wifiCallback = callback;
  }

  void onCpuFrequencyChange(std::function<void(int)> callback) {
    _cpuFrequencyCallback = callback;
  }

  void onPowerOff(std::function<void()> callback) {
    _powerOffCallback = callback;
  }

  void onLog(LogCallback callback) { _logCallback = callback; }

private:
  PowerState _state;
  unsigned long _lastActivityMs;
  int _savedBrightness;
  PowerTimeouts _timeouts;

  std::function<void(int)> _brightnessCallback;
  std::function<void(bool)> _wifiCallback;
  std::function<void(int)> _cpuFrequencyCallback;
  std::function<void()> _powerOffCallback;
  LogCallback _logCallback;

  bool canIdlePowerOff() const;
  void applyCpuFrequency(int mhz);
  void transitionTo(PowerState newState);
  void enterLightSleep();
  void logClient(const char *fmt, ...) const;
  void logServer(const char *fmt, ...) const;
};
