#include "Board.h"

#include "../Config.h"
#include "../diag/Log.h"
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_sleep.h>

namespace {
Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
    LCD_CS_PIN, LCD_SCLK_PIN, LCD_SDIO0_PIN, LCD_SDIO1_PIN, LCD_SDIO2_PIN,
    LCD_SDIO3_PIN);
Arduino_SH8601 *displayPanel =
    new Arduino_SH8601(displayBus, GFX_NOT_DEFINED, 0, SCREEN_WIDTH_PX,
                       SCREEN_HEIGHT_PX);

XPowersPMU pmu;
bool pmuReady = false;
bool pwrPressed = false;
unsigned long pwrPulseUntilMs = 0;
unsigned long lastPmuPollMs = 0;
uint8_t currentBrightness = DEFAULT_BRIGHTNESS;

void enablePmuAdc() {
  pmu.enableTemperatureMeasure();
  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
}

void pollPmuButton() {
  if (!pmuReady || millis() - lastPmuPollMs < 20) {
    return;
  }
  lastPmuPollMs = millis();

  pmu.getIrqStatus();
  if (pmu.isPekeyNegativeIrq()) {
    pwrPressed = true;
  }
  if (pmu.isPekeyPositiveIrq()) {
    pwrPressed = false;
  }
  if (pmu.isPekeyShortPressIrq()) {
    pwrPulseUntilMs = millis() + 180;
  }
  if (pmu.isPekeyLongPressIrq()) {
    pwrPulseUntilMs = millis() + 1200;
  }
  pmu.clearIrqStatus();
}
} // namespace

namespace Board {
bool init() {
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(AUDIO_PA_ENABLE_PIN, OUTPUT);
  digitalWrite(AUDIO_PA_ENABLE_PIN, HIGH);

  Wire.begin(BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
  Wire.setClock(400000);

  pmuReady =
      pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, BOARD_I2C_SDA_PIN,
                BOARD_I2C_SCL_PIN);
  if (pmuReady) {
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.setChargeTargetVoltage(3);
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_POSITIVE_IRQ |
                  XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ |
                  XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                  XPOWERS_AXP2101_PKEY_LONG_IRQ);
    enablePmuAdc();
    Log::client("Board", "AXP2101 online batt=%dmV pct=%d vbus=%dmV",
                pmu.getBattVoltage(), pmu.getBatteryPercent(),
                pmu.getVbusVoltage());
  } else {
    Log::client("Board", "AXP2101 not found");
  }

  return true;
}

void update() { pollPmuButton(); }

Arduino_SH8601 &display() { return *displayPanel; }

bool buttonAIsPressed() { return digitalRead(BUTTON_A_PIN) == LOW; }

bool buttonBIsPressed() {
  return pwrPressed || millis() < pwrPulseUntilMs;
}

void setDisplayBrightness(uint8_t brightness) {
  currentBrightness = brightness;
  if (brightness == 0) {
    displayPanel->displayOff();
  } else {
    displayPanel->displayOn();
    displayPanel->setBrightness(brightness);
  }
}

uint8_t displayBrightness() { return currentBrightness; }

int batteryLevel() {
  if (!pmuReady || !pmu.isBatteryConnect()) {
    return -1;
  }
  return pmu.getBatteryPercent();
}

uint16_t batteryVoltageMv() {
  if (!pmuReady) {
    return 0;
  }
  return pmu.getBattVoltage();
}

uint16_t vbusVoltageMv() {
  if (!pmuReady) {
    return 0;
  }
  return pmu.getVbusVoltage();
}

const char *powerSourceLabel() {
  if (!pmuReady) {
    return "?";
  }
  if (pmu.isVbusIn()) {
    return "USB";
  }
  if (pmu.isBatteryConnect()) {
    return "BAT";
  }
  return "?";
}

void powerOff() {
  if (pmuReady) {
    pmu.shutdown();
    delay(200);
  }
  esp_deep_sleep_start();
}
} // namespace Board
