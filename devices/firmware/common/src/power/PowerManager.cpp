#include "PowerManager.h"

#include "Config.h"
#include "hal/BoardPower.h"
#include <stdarg.h>

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
  case PowerState::LightSleep:
    return "LightSleep";
  case PowerState::PowerOff:
    return "PowerOff";
  default:
    return "Unknown";
  }
}

PowerManager::PowerManager()
    : _state(PowerState::Active), _lastActivityMs(millis()),
      _savedBrightness(DEFAULT_BRIGHTNESS),
      _timeouts({IDLE_DIM_MS, IDLE_SCREEN_OFF_MS, IDLE_LIGHT_SLEEP_MS,
                 IDLE_POWER_OFF_MS}) {}

void PowerManager::update() {
  if (_state == PowerState::Waking || _state == PowerState::PowerOff) {
    return;
  }

  const unsigned long idle = getIdleTime();

  PowerState target = PowerState::Active;
  if (idle >= _timeouts.powerOffMs && canIdlePowerOff()) {
    target = PowerState::PowerOff;
  } else if (Board::capabilities().lightSleep &&
             idle >= _timeouts.lightSleepMs) {
    target = PowerState::LightSleep;
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

void PowerManager::setTimeouts(const PowerTimeouts &timeouts) {
  _timeouts.dimMs = max(1000UL, timeouts.dimMs);
  _timeouts.screenOffMs = max(_timeouts.dimMs + 1000UL, timeouts.screenOffMs);
  _timeouts.lightSleepMs =
      max(_timeouts.screenOffMs + 1000UL, timeouts.lightSleepMs);
  const unsigned long powerOffFloor =
      Board::capabilities().lightSleep ? _timeouts.lightSleepMs
                                       : _timeouts.screenOffMs;
  _timeouts.powerOffMs = max(powerOffFloor + 1000UL, timeouts.powerOffMs);
  logServer("updated timeouts dim=%lu screen=%lu sleep=%lu off=%lu",
            _timeouts.dimMs, _timeouts.screenOffMs, _timeouts.lightSleepMs,
            _timeouts.powerOffMs);
}

void PowerManager::beginWaking() {
  if (!isInterruptible()) {
    return;
  }

  const PowerState previous = _state;
  _state = PowerState::Waking;
  _lastActivityMs = millis();

  logClient("%s -> Waking", powerStateName(previous));

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
  logClient("Waking -> Active");
}

void PowerManager::restoreActive() {
  if (_state == PowerState::Active || _state == PowerState::Waking) {
    return;
  }

  logClient("%s -> Active", powerStateName(_state));
  _state = PowerState::Active;
  applyCpuFrequency(CPU_ACTIVE_MHZ);

  if (_brightnessCallback) {
    _brightnessCallback(_savedBrightness);
  }
}

unsigned long PowerManager::getIdleTime() const {
  return millis() - _lastActivityMs;
}

bool PowerManager::canIdlePowerOff() const {
  if (IDLE_POWER_OFF_WHILE_USB_CONNECTED) {
    return true;
  }
  if (!Board::capabilities().usbPowerStatus) {
    return true;
  }
  return !Board::usbConnected();
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

  logClient("%s -> %s", powerStateName(oldState), powerStateName(newState));

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
    Board::setAudioAmpEnabled(false);
    if (_brightnessCallback) {
      _brightnessCallback(BRIGHTNESS_OFF);
    }
    break;

  case PowerState::LightSleep:
    enterLightSleep();
    break;

  case PowerState::PowerOff:
    applyCpuFrequency(CPU_IDLE_MHZ);
    Board::setAudioAmpEnabled(false);
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

void PowerManager::enterLightSleep() {
  logClient("entering light sleep");

  if (_brightnessCallback) {
    _brightnessCallback(BRIGHTNESS_OFF);
  }
  Board::setAudioAmpEnabled(false);
  if (_wifiCallback) {
    _wifiCallback(false);
  }

  while (true) {
    const LightSleepWakeReason reason =
        Board::enterLightSleep(LIGHT_SLEEP_WAKE_INTERVAL_MS);
    if (reason == LightSleepWakeReason::Button) {
      logClient("light sleep wake: button");
      if (_wifiCallback) {
        _wifiCallback(true);
      }
      _state = PowerState::Waking;
      _lastActivityMs = millis();
      if (_brightnessCallback) {
        _brightnessCallback(_savedBrightness);
      }
      return;
    }

    if (reason == LightSleepWakeReason::Timer &&
        getIdleTime() >= _timeouts.powerOffMs && canIdlePowerOff()) {
      transitionTo(PowerState::PowerOff);
      return;
    }
  }
}

void PowerManager::logClient(const char *fmt, ...) const {
  char message[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  if (_logCallback) {
    _logCallback('C', "Power", message);
  } else {
    Serial.printf("[Power] %s\n", message);
  }
}

void PowerManager::logServer(const char *fmt, ...) const {
  char message[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  if (_logCallback) {
    _logCallback('S', "Power", message);
  } else {
    Serial.printf("[Power] %s\n", message);
  }
}
