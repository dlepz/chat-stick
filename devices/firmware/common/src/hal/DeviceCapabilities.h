#pragma once

/**
 * @brief Hardware feature flags advertised by each board implementation.
 */
struct DeviceCapabilities {
  /// Whether the firmware can switch between internal and external speakers.
  bool externalSpeakerSwitch = false;

  /// Whether the firmware can adjust external speaker gain.
  bool externalSpeakerGain = false;

  /// Whether shared power management may use light sleep on this board.
  bool lightSleep = false;

  /// Whether battery percentage is available.
  bool batteryLevel = false;

  /// Whether battery voltage telemetry is available.
  bool batteryVoltage = false;

  /// Whether USB/VBUS power status is available.
  bool usbPowerStatus = false;

  /// Whether this board persists preferred server endpoint selection.
  bool endpointPreference = false;

  /// Whether boot log display is supported on this board.
  bool bootDisplay = false;

  /// Whether debug display text is supported on this board.
  bool debugDisplay = false;
};

/**
 * @brief Wake reasons returned after a light-sleep interval.
 */
enum class LightSleepWakeReason {
  /// Light sleep is not implemented on the active board.
  Unsupported,

  /// A wake-capable button was pressed.
  Button,

  /// The configured wake timer elapsed.
  Timer,

  /// The platform reported another wake reason.
  Other,
};

/**
 * @brief Wake reasons reported after booting from deep sleep.
 */
enum class DeepSleepWakeReason {
  /// The boot was not caused by a deep-sleep wake source.
  None,

  /// A wake-capable button resumed the device.
  Button,

  /// The configured deep-sleep timer elapsed.
  Timer,

  /// The platform reported another wake reason.
  Other,
};
