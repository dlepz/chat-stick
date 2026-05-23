#include "Board.h"

#include "../Config.h"
#include <M5PM1.h>
#include <time.h>

namespace {
M5PM1 pm1;
bool pm1Ready = false;
unsigned long lastPm1PollMs = 0;
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;
m5pm1_pwr_src_t currentPowerSource = M5PM1_PWR_SRC_UNKNOWN;

const DeviceCapabilities kCapabilities = {.externalSpeakerSwitch = true,
                                          .externalSpeakerGain = true,
                                          .lightSleep = false,
                                          .batteryLevel = true,
                                          .batteryVoltage = true,
                                          .usbPowerStatus = true,
                                          .endpointPreference = false,
                                          .bootDisplay = false,
                                          .debugDisplay = false};

const char *sourceLabel(m5pm1_pwr_src_t source) {
  switch (source) {
  case M5PM1_PWR_SRC_5VIN:
    return "USB";
  case M5PM1_PWR_SRC_5VINOUT:
    return "5Vout";
  case M5PM1_PWR_SRC_BAT:
    return "BAT";
  default:
    return "?";
  }
}
} // namespace

namespace Board {
bool init() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  const m5pm1_err_t pm1BeginRc = pm1.begin(&M5.In_I2C);
  if (pm1BeginRc == M5PM1_OK) {
    pm1Ready = true;
    pm1.setSingleResetDisable(true);
    const m5pm1_err_t chgRc = pm1.setChargeEnable(true);
    m5pm1_irq_btn_t drain;
    pm1.irqGetBtnStatusEnum(&drain, M5PM1_CLEAN_ALL);
    uint16_t vbat = 0, vin = 0;
    pm1.readVbat(&vbat);
    pm1.readVin(&vin);
    pm1.getPowerSource(&currentPowerSource);
    Serial.printf("[PM1] init OK; chargeEnable rc=%d vbat=%u vin=%u src=%d\n",
                  static_cast<int>(chgRc), vbat, vin,
                  static_cast<int>(currentPowerSource));
  } else {
    Serial.printf("[PM1] init failed rc=%d\n", static_cast<int>(pm1BeginRc));
  }

  currentBrightness = M5.Display.getBrightness();
  return true;
}

const DeviceCapabilities &capabilities() { return kCapabilities; }

void update() {
  M5.update();

  if (!pm1Ready || millis() - lastPm1PollMs <= 3000) {
    return;
  }

  lastPm1PollMs = millis();
  uint16_t vbat = 0, vin = 0, v5 = 0;
  pm1.readVbat(&vbat);
  pm1.readVin(&vin);
  pm1.read5VInOut(&v5);
  pm1.getPowerSource(&currentPowerSource);

  char ts[16];
  time_t now = time(nullptr);
  struct tm local;
  if (localtime_r(&now, &local) && local.tm_year + 1900 >= 2024) {
    strftime(ts, sizeof(ts), "%H:%M:%S", &local);
  } else {
    snprintf(ts, sizeof(ts), "up+%lus", millis() / 1000);
  }
  Serial.printf("[%s] [Pwr] vbat=%u mV level=%d vin=%u v5out=%u src=%s "
                "heap=%uK\n",
                ts, vbat, batteryLevel(), vin, v5,
                sourceLabel(currentPowerSource),
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
}

M5GFX &display() { return M5.Display; }

bool buttonAIsPressed() { return M5.BtnA.isPressed(); }

bool buttonBIsPressed() { return M5.BtnB.isPressed(); }

void setDisplayBrightness(uint8_t brightness) {
  currentBrightness = brightness;
  M5.Display.setBrightness(brightness);
}

uint8_t displayBrightness() { return currentBrightness; }

void setAudioAmpEnabled(bool) {}

int batteryLevel() { return M5.Power.getBatteryLevel(); }

uint16_t batteryVoltageMv() {
  if (!pm1Ready) {
    return 0;
  }
  uint16_t vbat = 0;
  pm1.readVbat(&vbat);
  return vbat;
}

uint16_t vbusVoltageMv() {
  if (!pm1Ready) {
    return 0;
  }
  uint16_t vin = 0;
  pm1.readVin(&vin);
  return vin;
}

bool usbConnected() {
  if (!pm1Ready) {
    return false;
  }
  pm1.getPowerSource(&currentPowerSource);
  return currentPowerSource == M5PM1_PWR_SRC_5VIN ||
         currentPowerSource == M5PM1_PWR_SRC_5VINOUT;
}

const char *powerSourceLabel() {
  if (!pm1Ready) {
    return "?";
  }
  pm1.getPowerSource(&currentPowerSource);
  return sourceLabel(currentPowerSource);
}

LightSleepWakeReason enterLightSleep(unsigned long wakeIntervalMs) {
  delay(wakeIntervalMs);
  return LightSleepWakeReason::Unsupported;
}

void powerOff() {
  if (pm1Ready) {
    const m5pm1_err_t rc = pm1.shutdown();
    Serial.printf("[Power] PM1 shutdown rc=%d\n", static_cast<int>(rc));
    delay(200);
  }
  M5.Power.powerOff();
}
} // namespace Board
