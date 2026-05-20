#include "PowerManager.h"

#include "../Config.h"

const char *powerStateName(PowerState state) {
  switch (state) {
  case PowerState::Active:
    return "Active";
  case PowerState::Dimmed:
    return "Dimmed";
  case PowerState::ScreenOff:
    return "ScreenOff";
  case PowerState::Waking:
    return "Waking";
  case PowerState::PowerOff:
    return "PowerOff";
  default:
    return "Unknown";
  }
}

PowerManager::PowerManager()
    : _state(PowerState::Active), _lastActivityMs(millis()),
      _savedBrightness(DEFAULT_BRIGHTNESS),
      _timeouts({IDLE_DIM_MS, IDLE_SCREEN_OFF_MS, IDLE_POWER_OFF_MS}) {}

void PowerManager::update() {
  if (_state == PowerState::Waking || _state == PowerState::PowerOff) {
    return;
  }

  const unsigned long idle = getIdleTime();

  PowerState target = PowerState::Active;
  if (idle >= _timeouts.powerOffMs) {
    target = PowerState::PowerOff;
  } else if (idle >= _timeouts.screenOffMs) {
    target = PowerState::ScreenOff;
  } else if (idle >= _timeouts.dimMs) {
    target = PowerState::Dimmed;
  }

  if (target > _state) {
    transitionTo(target);
  }
}

void PowerManager::registerActivity() {
  _lastActivityMs = millis();

  if (isInterruptible()) {
    restoreActive();
  }
}

void PowerManager::beginWaking() {
  if (!isInterruptible()) {
    return;
  }

  const PowerState previous = _state;
  _state = PowerState::Waking;
  _lastActivityMs = millis();

  Serial.printf("[Power] %s -> Waking\n", powerStateName(previous));

  applyCpuFrequency(CPU_ACTIVE_MHZ);

  if (_brightnessCallback) {
    _brightnessCallback(_savedBrightness);
  }
}

void PowerManager::finishWaking() {
  if (_state != PowerState::Waking) {
    return;
  }

  _state = PowerState::Active;
  _lastActivityMs = millis();
  applyCpuFrequency(CPU_ACTIVE_MHZ);
  Serial.println("[Power] Waking -> Active");
}

void PowerManager::restoreActive() {
  if (_state == PowerState::Active || _state == PowerState::Waking) {
    return;
  }

  Serial.printf("[Power] %s -> Active\n", powerStateName(_state));
  _state = PowerState::Active;
  applyCpuFrequency(CPU_ACTIVE_MHZ);

  if (_brightnessCallback) {
    _brightnessCallback(_savedBrightness);
  }
}

unsigned long PowerManager::getIdleTime() const {
  return millis() - _lastActivityMs;
}

void PowerManager::applyCpuFrequency(int mhz) {
  if (_cpuFrequencyCallback) {
    _cpuFrequencyCallback(mhz);
  }
}

void PowerManager::transitionTo(PowerState newState) {
  if (newState == _state) {
    return;
  }

  const PowerState oldState = _state;
  _state = newState;

  Serial.printf("[Power] %s -> %s\n", powerStateName(oldState),
                powerStateName(newState));

  switch (newState) {
  case PowerState::Active:
    applyCpuFrequency(CPU_ACTIVE_MHZ);
    if (_brightnessCallback) {
      _brightnessCallback(_savedBrightness);
    }
    break;

  case PowerState::Dimmed:
    applyCpuFrequency(CPU_ACTIVE_MHZ);
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_DIM);
    }
    break;

  case PowerState::ScreenOff:
    applyCpuFrequency(CPU_IDLE_MHZ);
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_OFF);
    }
    break;

  case PowerState::PowerOff:
    applyCpuFrequency(CPU_IDLE_MHZ);
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_OFF);
    }
    if (_wifiCallback) {
      _wifiCallback(false);
    }
    if (_powerOffCallback) {
      _powerOffCallback();
    }
    break;

  case PowerState::Waking:
    break;
  }
}
