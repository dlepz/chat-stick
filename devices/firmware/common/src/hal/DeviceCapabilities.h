#pragma once

struct DeviceCapabilities {
  bool externalSpeakerSwitch = false;
  bool externalSpeakerGain = false;
  bool lightSleep = false;
  bool batteryLevel = false;
  bool batteryVoltage = false;
  bool usbPowerStatus = false;
  bool endpointPreference = false;
  bool bootDisplay = false;
  bool debugDisplay = false;
};

enum class LightSleepWakeReason {
  Unsupported,
  Button,
  Timer,
  Other,
};
