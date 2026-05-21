#include "AppController.h"

#include "../Config.h"
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <time.h>

namespace {
constexpr AppController::AlarmStep kAlarmPatternData[] = {
    {1760, 90}, {2349, 90}, {1760, 90}, {2349, 90}, {1760, 200},
    {0, 140},
    {1760, 90}, {2349, 90}, {1760, 90}, {2349, 90}, {1760, 200},
    {0, 2000},
};
constexpr int kAlarmPatternCount =
    sizeof(kAlarmPatternData) / sizeof(kAlarmPatternData[0]);
} // namespace

const AppController::AlarmStep *AppController::alarmPattern() {
  return kAlarmPatternData;
}

int AppController::alarmPatternLen() { return kAlarmPatternCount; }

void AppController::setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== M5 Live Voice Assistant ===");
  Serial.flush();

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  setenv("TZ", LOCAL_TZ, 1);
  tzset();

  _settings.init();
  _timers.init();

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER || wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.printf("[Boot] Wake from deep sleep cause=%d\n",
                  static_cast<int>(wakeCause));
  }

  const m5pm1_err_t pm1BeginRc = _pm1.begin(&M5.In_I2C);
  if (pm1BeginRc == M5PM1_OK) {
    _pm1Ready = true;
    _pm1.setSingleResetDisable(true);
    const m5pm1_err_t chgRc = _pm1.setChargeEnable(true);
    m5pm1_irq_btn_t drain;
    _pm1.irqGetBtnStatusEnum(&drain, M5PM1_CLEAN_ALL);
    uint16_t vbat = 0, vin = 0;
    m5pm1_pwr_src_t src = M5PM1_PWR_SRC_UNKNOWN;
    _pm1.readVbat(&vbat);
    _pm1.readVin(&vin);
    _pm1.getPowerSource(&src);
    Serial.printf("[PM1] init OK; chargeEnable rc=%d vbat=%u vin=%u src=%d\n",
                  static_cast<int>(chgRc), vbat, vin, static_cast<int>(src));
  } else {
    Serial.printf("[PM1] init failed rc=%d\n", static_cast<int>(pm1BeginRc));
  }

  _display.init();
  _display.setBrightness(_settings.brightness());
  _startupPowerDone = true;
  _screenDirty = true;
  renderIfNeeded();

  _powerManager.setSavedBrightness(_settings.brightness());
  configureCallbacks();

  _wifi.init();

  _audio.setExternalSpeakerGain(_settings.externalSpeakerGain());
  _audio.setUseExternalSpeaker(_settings.useExternalSpeaker());
  if (!_audio.init()) {
    setErrorState(ErrorCategory::Startup, "Startup failed",
                  "Audio buffer unavailable");
    renderIfNeeded();
    return;
  }

  _audio.setVolume(_settings.volume());
  if (!_settings.chatId().isEmpty()) {
    _settings.clearChatId();
  }
  _chatId = "";
  _live.setChatId("");
  _live.setVoice(_settings.voice());

  LiveSessionCallbacks callbacks;
  callbacks.onActivity = [this]() { _powerManager.registerActivity(); };
  callbacks.onStatus = [this](const String &status) {
    if (_appState != AppState::Error) {
      setAppState(AppState::Connecting, status);
    }
  };
  callbacks.onServerReady = [this]() {
    if (_appState == AppState::Connecting) {
      _startupChecklistVisible = false;
      _appRegion = AppRegion::Chat;
      setAppState(AppState::Ready, "Ready");
    }
  };
  callbacks.onReady = [this]() {
    if (_appState == AppState::Connecting) {
      _startupChecklistVisible = false;
      _appRegion = AppRegion::Chat;
      setAppState(AppState::Ready, "Ready");
    }
  };
  callbacks.onTurnComplete = [this]() {
    // Ignore turnComplete signals that arrive before any audio for the current
    // turn — they're stale from a prior turn that the user interrupted.
    if (_turnHasAudio) {
      _turnComplete = true;
    }
  };
  callbacks.onDropAudio = [this]() {
    // Gemini detected a user interrupt mid-response. Flush any queued tail of
    // the prior turn so it doesn't play into the new one. Don't change state —
    // the user is mid-recording; release will drive the next transition.
    _audio.stopPlayback();
    _turnComplete = false;
    _turnHasAudio = false;
  };
  callbacks.onChatId = [this](const String &chatId) {
    _chatId = chatId;
    _live.setChatId(chatId);
    _screenDirty = true;
  };
  callbacks.onConversationReset = [this]() {
    _audio.stopPlayback();
    setToolTextImmediate("");
    if (_imagePresent) {
      _display.clearImage();
      _imagePresent = false;
    }
    _turnComplete = false;
    _turnHasAudio = false;
    _pendingTurnReset = false;
    resetBodyPage();
    setAppState(AppState::Ready, "Ready");
    _screenDirty = true;
  };
  callbacks.onShowText = [this](const String &text) {
    _pendingTurnReset = false;
    startToolTextReveal(text);
  };
  auto applyPendingTurnReset = [this]() {
    if (!_pendingTurnReset) {
      return;
    }
    setToolTextImmediate("");
    if (_imagePresent) {
      _display.clearImage();
      _imagePresent = false;
    }
    _pendingTurnReset = false;
  };
  callbacks.onShowImagePending = [this, applyPendingTurnReset]() {
    applyPendingTurnReset();
    _screenDirty = true;
  };
  callbacks.onShowImage = [this, applyPendingTurnReset](
                              const uint8_t *packed, size_t packedLen,
                              int width, int height) {
    applyPendingTurnReset();
    if (_display.setImage(packed, packedLen, width, height)) {
      _imagePresent = true;
      resetBodyPage();
      _screenDirty = true;
    }
  };
  callbacks.onShowImageFailed = [this]() {
    if (_imagePresent) {
      _display.clearImage();
      _imagePresent = false;
      resetBodyPage();
      _screenDirty = true;
    }
  };
  callbacks.onTranscript = [this, applyPendingTurnReset](
                               const String &source, const String &text) {
    if (source != "model") {
      return;
    }
    applyPendingTurnReset();
    appendToolTextReveal(text);
  };
  callbacks.onError = [this](const String &category, const String &error) {
    const ErrorCategory mapped = category == "gemini_unavailable"
                                     ? ErrorCategory::GeminiUnavailable
                                     : ErrorCategory::ServerRefused;
    setErrorState(mapped, "Server error", error);
  };
  callbacks.onIgnoredAudio = [this](const String &) {
    _turnComplete = false;
    _turnHasAudio = false;
    _pendingTurnReset = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
  };
  callbacks.onAudio = [this](const uint8_t *data, size_t len) {
    if (_appState != AppState::Recording) {
      _audio.queuePlayback(data, len);
      _turnHasAudio = true;
    }
  };
  callbacks.onBrightness = [this](int level) {
    _display.setBrightness(level);
    _powerManager.setSavedBrightness(level);
    _settings.setBrightness(level);
    _screenDirty = true;
  };
  callbacks.onVolume = [this](int level) {
    _audio.setVolume(level);
    _settings.setVolume(level);
  };
  callbacks.onSetSpeaker = [this](const String &mode) {
    bool enabled;
    if (mode.equalsIgnoreCase("external") || mode.equalsIgnoreCase("spk2") ||
        mode.equalsIgnoreCase("hat")) {
      enabled = true;
    } else if (mode.equalsIgnoreCase("internal") ||
               mode.equalsIgnoreCase("builtin") ||
               mode.equalsIgnoreCase("stick")) {
      enabled = false;
    } else {
      return false;
    }
    _settings.setUseExternalSpeaker(enabled);
    _audio.setUseExternalSpeaker(enabled);
    return true;
  };
  callbacks.onSetExternalGain = [this](int gain) {
    if (gain < SettingsStore::kMinExternalGain ||
        gain > SettingsStore::kMaxExternalGain) {
      return false;
    }
    _settings.setExternalSpeakerGain(gain);
    _audio.setExternalSpeakerGain(_settings.externalSpeakerGain());
    return true;
  };
  callbacks.onPlaySound = [this](const String &sound) {
    _powerManager.registerActivity();
    return _audio.playNamedSound(sound);
  };
  callbacks.onPlayMelody = [this](const String &notes) {
    _powerManager.registerActivity();
    return _audio.playMelody(notes);
  };
  callbacks.onPowerOff = [this]() { performPowerOff(); };
  callbacks.getDeviceStatusJson = [this]() { return deviceStatusJson(); };
  callbacks.onVoiceChanged = [this](const String &voice) {
    _settings.setVoice(voice);
  };
  callbacks.onSetTimer = [this](int duration, const String &name) {
    return handleSetTimerTool(duration, name);
  };
  callbacks.onListTimers = [this]() { return handleListTimersTool(); };
  callbacks.onCancelTimer = [this](const TimerRef &ref, bool all) {
    return handleCancelTimerTool(ref, all);
  };
  callbacks.onExtendTimer = [this](int delta, const TimerRef &ref) {
    return handleExtendTimerTool(delta, ref);
  };

  _live.init(callbacks);

  Serial.printf("Capture: %d Hz, %d ms chunks (%u bytes)\n", MIC_SAMPLE_RATE,
                MIC_CHUNK_MS, static_cast<unsigned>(_audio.captureBytes()));
  Serial.printf("Playback: %d Hz, max %d s\n", PLAY_SAMPLE_RATE,
                MAX_PLAYBACK_SEC);

  renderIfNeeded();

  // If a timer has already expired (e.g. we just woke from deep sleep on its
  // deadline), enter the alarm state before bringing up the network — the
  // trill needs the main loop to run, so we can't block on WiFi.
  checkTimerExpiry();
  if (_appState != AppState::Alarm) {
    _networkStackStarted = true;
    connectNetworkStack();
    renderIfNeeded();
  } else {
    _bootHadExpiredAlarm = true;
  }

  setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
  Serial.printf("[Setup] CPU clock set to %lu MHz\n", getCpuFrequencyMhz());
}

void AppController::loop() {
  M5.update();

  if (_pm1Ready && millis() - _lastPm1PollMs > 3000) {
    _lastPm1PollMs = millis();
    uint16_t vbat = 0, vin = 0, v5 = 0;
    m5pm1_pwr_src_t src = M5PM1_PWR_SRC_UNKNOWN;
    _pm1.readVbat(&vbat);
    _pm1.readVin(&vin);
    _pm1.read5VInOut(&v5);
    _pm1.getPowerSource(&src);
    const int level = M5.Power.getBatteryLevel();
    const char *srcLabel = src == M5PM1_PWR_SRC_5VIN      ? "USB"
                           : src == M5PM1_PWR_SRC_5VINOUT ? "5Vout"
                           : src == M5PM1_PWR_SRC_BAT     ? "BAT"
                                                          : "?";
    char ts[16];
    time_t now = time(nullptr);
    struct tm local;
    if (localtime_r(&now, &local) && local.tm_year + 1900 >= 2024) {
      strftime(ts, sizeof(ts), "%H:%M:%S", &local);
    } else {
      snprintf(ts, sizeof(ts), "up+%lus", millis() / 1000);
    }
    Serial.printf("[%s] [Pwr] vbat=%u mV level=%d vin=%u v5out=%u src=%s heap=%uK\n",
                  ts, vbat, level, vin, v5, srcLabel,
                  static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  }

  if (millis() - _lastHeartbeatMs > 3000) {
    _lastHeartbeatMs = millis();
    Serial.printf("[Loop] state=%d region=%d power=%s ws=%d\n",
                  static_cast<int>(_appState), static_cast<int>(_appRegion),
                  powerStateName(_powerManager.getState()), _live.isConnected());
  }

  if (millis() - _lastHeaderRefreshMs > 30000) {
    _lastHeaderRefreshMs = millis();
    _screenDirty = true;
  }

  _wifi.poll();
  _live.poll();
  _live.reconnectIfNeeded(_wifi.isConnected() &&
                          _appState != AppState::Error);

  checkTimerExpiry();

  handleButtons();
  if (_appState != AppState::Alarm) {
    processRecording();
    processPlayback();
    processThinkingTimeout();
    processTextReveal();
    processPower();
    processCaptivePortal();
  } else {
    serviceAlarmTrill();
  }
  renderIfNeeded();
  delay(1);
}

void AppController::configureCallbacks() {
  _powerManager.onBrightnessChange(
      [this](int brightness) { _display.setBrightness(brightness); });

  _powerManager.onWiFiStateChange(
      [this](bool enabled) { setNetworkEnabled(enabled); });

  _powerManager.onCpuFrequencyChange([](int mhz) {
    if (static_cast<int>(getCpuFrequencyMhz()) == mhz) {
      return;
    }
    setCpuFrequencyMhz(mhz);
    Serial.printf("[Power] CPU clock set to %lu MHz\n", getCpuFrequencyMhz());
  });

  _powerManager.onPowerOff([this]() { performPowerOff(); });
}

void AppController::connectNetworkStack() {
  _appRegion = AppRegion::Chat;
  _startupChecklistVisible = true;
  _startupPowerDone = true;
  _startupWifiDone = false;
  _startupInternetDone = false;
  setAppState(AppState::Connecting, "Starting...");
  renderIfNeeded();

  if (!_wifi.connectKnownNetworks()) {
    _startupChecklistVisible = false;
    setErrorState(ErrorCategory::WiFiTimeout, "WiFi failed",
                  "A retry  B hold menu");
    return;
  }

  _startupWifiDone = true;
  _screenDirty = true;
  renderIfNeeded();

  if (!_live.pingServer()) {
    _startupChecklistVisible = false;
    setErrorState(ErrorCategory::ServerRefused, "Internet failed",
                  "A retry  B hold menu");
    return;
  }

  _startupInternetDone = true;
  _screenDirty = true;
  renderIfNeeded();

  // Open only the lightweight device channel on boot. The server waits to open
  // Gemini Live until the user's first recording starts.
  _live.connect();
}

void AppController::setNetworkEnabled(bool enabled) {
  if (enabled) {
    setAppState(AppState::Connecting, "Waking...");
    connectNetworkStack();
    return;
  }

  _live.disconnect();
  _wifi.disconnect();
}

void AppController::setAppState(AppState state, const String &status,
                                const String &error) {
  _appState = state;
  if (state != AppState::Connecting) {
    _startupChecklistVisible = false;
  }
  if (state != AppState::Error) {
    _errorCategory = ErrorCategory::None;
  }
  if (!status.isEmpty()) {
    _statusText = status;
  }
  _errorText = error;
  resetBodyPage();
  _screenDirty = true;
}

void AppController::setErrorState(ErrorCategory category, const String &status,
                                  const String &error) {
  _startupChecklistVisible = false;
  _errorCategory = category;
  _appState = AppState::Error;
  _statusText = status;
  _errorText = error;
  _appRegion = AppRegion::Chat;
  resetBodyPage();
  _screenDirty = true;
}

void AppController::retryAfterError() {
  switch (_errorCategory) {
  case ErrorCategory::Startup:
    ESP.restart();
    return;
  case ErrorCategory::WiFiTimeout:
  case ErrorCategory::ServerRefused:
  case ErrorCategory::GeminiUnavailable:
  case ErrorCategory::None:
  default:
    connectNetworkStack();
    return;
  }
}

void AppController::performPowerOff() {
  if (maybeDeepSleepUntilNextTimer()) {
    return; // doesn't return — device deep-sleeps until next deadline
  }

  Serial.println("[Power] Powering off");
  _live.disconnect();
  _wifi.disconnect();
  delay(100);
  if (_pm1Ready) {
    const m5pm1_err_t rc = _pm1.shutdown();
    Serial.printf("[Power] PM1 shutdown rc=%d\n", static_cast<int>(rc));
    delay(200);
  }
  M5.Power.powerOff();
}

bool AppController::maybeDeepSleepUntilNextTimer() {
  if (_timers.count() == 0) {
    return false;
  }

  const time_t now = time(nullptr);
  const time_t deadline = _timers.nextDeadline();
  if (now < TIMER_MIN_VALID_EPOCH || deadline == 0) {
    return false;
  }

  // If a timer has already lapsed, don't sleep — fire the alarm now.
  if (deadline <= now) {
    checkTimerExpiry();
    return false;
  }

  const uint64_t sleepUs =
      static_cast<uint64_t>(deadline - now) * 1000000ULL;

  Serial.printf("[Power] Deep sleep %llu us until next timer\n",
                static_cast<unsigned long long>(sleepUs));

  _live.disconnect();
  _wifi.disconnect();
  _audio.stopPlayback();
  M5.Display.setBrightness(0);
  delay(100);

  esp_sleep_enable_timer_wakeup(sleepUs);
  // Wake on either button (active-low). Both pins are RTC-capable on ESP32-S3.
  rtc_gpio_pullup_en(BUTTON_A_PIN);
  rtc_gpio_pulldown_dis(BUTTON_A_PIN);
  rtc_gpio_pullup_en(BUTTON_B_PIN);
  rtc_gpio_pulldown_dis(BUTTON_B_PIN);
  const uint64_t buttonMask =
      (1ULL << BUTTON_A_PIN) | (1ULL << BUTTON_B_PIN);
  esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ANY_LOW);

  esp_deep_sleep_start();
  return true; // not reached
}

void AppController::clearToolText() {
  setToolTextImmediate("");
  if (_imagePresent) {
    _display.clearImage();
    _imagePresent = false;
  }
  resetBodyPage();
}

void AppController::setToolTextImmediate(const String &text) {
  cancelToolTextReveal();
  _toolText = text;
  _toolTextRevealTarget = text;
  _toolTextRevealLayout = _display.layoutTextForReveal(text);
  _toolTextRevealIndex = static_cast<int>(_toolTextRevealLayout.length());
  resetBodyPage();
  _screenDirty = true;
}

void AppController::startToolTextReveal(const String &text) {
  _toolTextRevealTarget = text;
  rebuildToolTextRevealLayout();
  _toolText = "";
  _toolTextRevealIndex = 0;
  _lastTextRevealMs = 0;
  resetBodyPage();
  _screenDirty = true;
}

void AppController::appendToolTextReveal(const String &text) {
  if (text.isEmpty()) {
    return;
  }

  if (_toolTextRevealTarget.isEmpty() && !_toolText.isEmpty()) {
    _toolTextRevealTarget = _toolText;
  }
  _toolTextRevealTarget += text;
  rebuildToolTextRevealLayout();
  resetBodyPage();
  _screenDirty = true;
}

void AppController::rebuildToolTextRevealLayout() {
  _toolTextRevealLayout = _display.layoutTextForReveal(_toolTextRevealTarget);
  const int targetLen = static_cast<int>(_toolTextRevealLayout.length());
  _toolTextRevealIndex = constrain(_toolTextRevealIndex, 0, targetLen);
  _toolText = _toolTextRevealLayout.substring(0, _toolTextRevealIndex);
  _lastTextRevealMs = 0;
}

void AppController::completeToolTextReveal() {
  if (_toolTextRevealIndex >= static_cast<int>(_toolTextRevealLayout.length())) {
    return;
  }

  _toolTextRevealIndex = static_cast<int>(_toolTextRevealLayout.length());
  _toolText = _toolTextRevealLayout;
  _screenDirty = true;
}

void AppController::cancelToolTextReveal() {
  _toolTextRevealTarget = "";
  _toolTextRevealLayout = "";
  _toolTextRevealIndex = 0;
  _lastTextRevealMs = 0;
}

void AppController::resetBodyPage() { _bodyPageIndex = 0; }

void AppController::handleButtons() {
  const unsigned long now = millis();
  _buttonA.update(M5.BtnA.isPressed(), now);
  _buttonB.update(M5.BtnB.isPressed(), now);

  if (_appState == AppState::Alarm) {
    if (handleAlarmButtons()) {
      return;
    }
  }

  if (_appState == AppState::ConfirmReset) {
    if (_buttonA.consumeClick()) {
      beginFactoryReset();
      return;
    }

    if (_buttonB.consumeClick()) {
      _appState = _resetReturnState;
      _statusText = _resetReturnStatus;
      _errorText = _resetReturnError;
      _errorCategory = _resetReturnCategory;
      _appRegion = AppRegion::Chat;
      _screenDirty = true;
      return;
    }
  }

  if (_buttonA.isPressed() && _buttonB.isPressed()) {
    if (_resetHoldStartMs == 0) {
      _resetHoldStartMs = now;
    } else if (now - _resetHoldStartMs >= kResetHoldMs &&
               _appState != AppState::Recording &&
               _appState != AppState::ConfirmReset) {
      _resetReturnState = _appState;
      _resetReturnStatus = _statusText;
      _resetReturnError = _errorText;
      _resetReturnCategory = _errorCategory;
      setAppState(AppState::ConfirmReset, "Factory reset?");
      _errorText = "A confirm  B cancel";
      _screenDirty = true;
      return;
    }
  } else {
    _resetHoldStartMs = 0;
  }

  if (_powerManager.isInterruptible() &&
      (_buttonA.consumePressed() || _buttonB.consumePressed())) {
    completeToolTextReveal();
    _powerManager.beginWaking();
    _screenDirty = true;
  }

  if (_powerManager.isWaking()) {
    if (_buttonA.consumeReleased() || _buttonB.consumeReleased()) {
      _powerManager.finishWaking();
      _screenDirty = true;
    }
    _buttonA.clearEvents();
    _buttonB.clearEvents();
    return;
  }

  if (_appRegion == AppRegion::Menu) {
    handleMenuButtons();
    return;
  }

  handleChatButtons();
}

void AppController::handleChatButtons() {
  if (_appState == AppState::Error && _buttonA.consumeClick()) {
    retryAfterError();
    return;
  }

  if (_buttonB.consumeHoldStart() && _appState != AppState::Recording) {
    completeToolTextReveal();
    openMenu(_appState == AppState::Error ? MenuState::Device : MenuState::Home);
    return;
  }

  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    completeToolTextReveal();
    const int pageCount = currentBodyPageCount();
    if (pageCount > 1) {
      _bodyPageIndex = (_bodyPageIndex + 1) % pageCount;
    } else if (!_toolText.isEmpty() || _imagePresent) {
      clearToolText();
    }
    _screenDirty = true;
  }

  if (_buttonA.consumePressed() &&
      (_appState == AppState::Ready || _appState == AppState::Playing ||
       _appState == AppState::Thinking)) {
    completeToolTextReveal();
    startRecording();
    return;
  }

  if (_buttonA.consumeReleased() && _appState == AppState::Recording) {
    stopRecording();
  }
}

void AppController::handleMenuButtons() {
  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    cycleMenuSelection();
    return;
  }

  if (_buttonB.consumeHoldStart()) {
    _powerManager.registerActivity();
    navigateBackFromMenu();
    return;
  }

  if (_buttonA.consumeClick()) {
    _powerManager.registerActivity();
    selectCurrentMenuItem();
  }
}

void AppController::openMenu(MenuState state) {
  completeToolTextReveal();
  _appRegion = AppRegion::Menu;
  _menuState = state;
  _menuSelection = 0;
  if (state == MenuState::ResumeChat) {
    loadConversationHistory();
  }
  _screenDirty = true;
}

void AppController::closeMenu() {
  _appRegion = AppRegion::Chat;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  _screenDirty = true;
}

void AppController::navigateBackFromMenu() {
  if (_menuState == MenuState::Home) {
    closeMenu();
    return;
  }

  openMenu(MenuState::Home);
}

void AppController::cycleMenuSelection() {
  const int count = menuItemCount();
  if (count <= 0) {
    return;
  }
  _menuSelection = (_menuSelection + 1) % count;
  _screenDirty = true;
}

void AppController::selectCurrentMenuItem() {
  switch (_menuState) {
  case MenuState::Home:
    switch (_menuSelection) {
    case 0:
      closeMenu();
      return;
    case 1:
      closeMenu();
      startFreshConversation();
      return;
    case 2:
      openMenu(MenuState::ResumeChat);
      return;
    case 3:
      openMenu(MenuState::Device);
      return;
    default:
      return;
    }

  case MenuState::Device:
    switch (_menuSelection) {
    case 0:
      openMenu(MenuState::Home);
      return;
    case 1:
      closeMenu();
      startCaptivePortalFlow();
      return;
    case 2:
      closeMenu();
      checkForUpdates();
      return;
    case 3: {
      const bool next = !_settings.useExternalSpeaker();
      _settings.setUseExternalSpeaker(next);
      _audio.setUseExternalSpeaker(next);
      return;
    }
    case 4:
      performPowerOff();
      return;
    default:
      return;
    }

  case MenuState::ResumeChat:
    if (_menuSelection == 0) {
      openMenu(MenuState::Home);
      return;
    }
    if (_historyCount == 0) {
      return;
    }
    resumeConversation(_menuSelection - 1);
    return;
  }
}

int AppController::menuItemCount() const {
  switch (_menuState) {
  case MenuState::Home:
    return 4;
  case MenuState::Device:
    return 5;
  case MenuState::ResumeChat:
    return _historyCount > 0 ? 1 + _historyCount : 2;
  }
  return 0;
}

String AppController::menuItemLabel(int index) const {
  switch (_menuState) {
  case MenuState::Home:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return "New conversation";
    case 2:
      return "Resume chat";
    case 3:
      return "Device";
    default:
      return "";
    }

  case MenuState::Device:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return "Set up WiFi";
    case 2:
      return "Check for updates";
    case 3:
      return _settings.useExternalSpeaker() ? "Speaker: external"
                                            : "Speaker: internal";
    case 4:
      return "Turn off";
    default:
      return "";
    }

  case MenuState::ResumeChat:
    if (index == 0) {
      return "Go back";
    }
    if (_historyCount == 0 && index == 1) {
      return _toolText.isEmpty() ? "No saved chats" : _toolText.substring(0, 26);
    }
    if (index - 1 < _historyCount) {
      const ConversationSummary &entry = _history[index - 1];
      const String preview =
          entry.lastMessage.isEmpty() ? entry.chatId : entry.lastMessage;
      return preview.substring(0, 26);
    }
    return "";
  }

  return "";
}

void AppController::loadConversationHistory() {
  _historyCount = 0;
  if (!_wifi.isConnected()) {
    setToolTextImmediate("Connect WiFi first");
    return;
  }

  if (!_live.fetchConversationHistory(_history, kMaxConversationHistory,
                                      _historyCount)) {
    setToolTextImmediate("History unavailable");
    return;
  }

  if (_historyCount == 0) {
    setToolTextImmediate("No saved chats");
  }
}

void AppController::resumeConversation(int index) {
  if (index < 0 || index >= _historyCount) {
    return;
  }

  const ConversationSummary &entry = _history[index];
  _chatId = entry.chatId;
  _live.setChatId(_chatId);
  setToolTextImmediate(entry.lastMessage);
  if (_imagePresent) {
    _display.clearImage();
    _imagePresent = false;
  }
  resetBodyPage();
  closeMenu();
  _live.disconnect();
  _startupChecklistVisible = false;
  setAppState(AppState::Connecting, "Restoring...");
  _live.connect();
}

void AppController::startFreshConversation() {
  _chatId = "";
  _settings.clearChatId();
  _live.setChatId("");
  clearToolText();
  _live.disconnect();
  _startupChecklistVisible = false;
  setAppState(AppState::Connecting, "New chat...");
  _live.connect();
}

void AppController::startCaptivePortalFlow() {
  _live.disconnect();
  _startupChecklistVisible = false;
  if (_wifi.startCaptivePortal()) {
    setAppState(AppState::Connecting, "WiFi setup");
    setToolTextImmediate("Join AP\n" + _wifi.captivePortalSsid() + "\nOpen " +
                         _wifi.captivePortalIp() + "\nSubmit WiFi form");
  } else {
    setErrorState(ErrorCategory::WiFiTimeout, "Portal failed",
                  "Could not start setup AP");
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::checkForUpdates() {
  FirmwareUpdateInfo info;
  if (!_wifi.isConnected()) {
    setToolTextImmediate("Offline\nCannot check updates");
  } else if (_live.checkFirmwareUpdate(info)) {
    if (info.available) {
      if (info.downloadUrl.isEmpty()) {
        setToolTextImmediate("Update unavailable\nNo download URL");
      } else {
        setAppState(AppState::Connecting, "Updating...");
        setToolTextImmediate("Downloading update\nv" +
                             String(info.latestVersion) + "\nPlease wait");
        renderIfNeeded();

        String error;
        if (_live.downloadAndApplyFirmwareUpdate(info.downloadUrl, error)) {
          setToolTextImmediate("Update installed\nRestarting...");
          renderIfNeeded();
          delay(500);
          ESP.restart();
        }

        setToolTextImmediate("Update failed\n" + error);
        setAppState(AppState::Ready, "Ready");
      }
    } else {
      setToolTextImmediate("Up to date\nv" + String(FIRMWARE_VERSION));
    }
  } else {
    setToolTextImmediate("Update check failed");
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::startRecording() {
  Serial.println("[Rec] === START RECORDING ===");
  _powerManager.registerActivity();
  _audio.stopPlayback();
  _audio.startRecording();
  _turnComplete = false;
  _turnHasAudio = false;
  _audioChunksSent = 0;
  _recordingStartMs = millis();
  _live.sendStart();
  setAppState(AppState::Recording, "Listening...");
  renderIfNeeded();
}

void AppController::stopRecording() {
  Serial.printf("[Rec] === STOP RECORDING === (sent %d chunks)\n",
                _audioChunksSent);
  _audio.stopRecording();
  _live.sendStop();
  _pendingTurnReset = true;
  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Thinking...");
}

void AppController::processRecording() {
  if (_appState != AppState::Recording) {
    return;
  }

  if (millis() - _recordingStartMs >= kMaxRecordingMs) {
    Serial.println("[Rec] Max recording time reached");
    stopRecording();
    return;
  }

  if (!_audio.captureChunk()) {
    Serial.println("[Rec] Mic.record() returned false");
    return;
  }

  const bool sent =
      _live.sendAudio(_audio.captureData(), _audio.captureBytes());
  _powerManager.registerActivity();
  _audioChunksSent++;
  _screenDirty = true;
  if (_audioChunksSent <= 5 || _audioChunksSent % 10 == 0) {
    Serial.printf("[Rec] #%d sent=%d\n", _audioChunksSent, sent);
  }
}

void AppController::processPlayback() {
  if (_appState != AppState::Thinking && _appState != AppState::Playing) {
    return;
  }

  const int buffered = _audio.bufferedPlaybackBytes();
  if (!_audio.playbackStarted() && buffered >= kMinPlaybackBytes) {
    _audio.markPlaybackStarted();
    setAppState(AppState::Playing, "Speaking...");
    _audio.advancePlayback();
  }

  if (_audio.playbackStarted()) {
    _audio.advancePlayback();
  }

  // Only exit to Ready from Playing — a turnComplete that arrives while we're
  // still Thinking (e.g. a stale signal from a prior, interrupted turn) must
  // not short-circuit waiting for the new response's audio.
  if (_appState == AppState::Playing && _turnComplete &&
      _audio.playbackIdle()) {
    _turnComplete = false;
    _turnHasAudio = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processThinkingTimeout() {
  if (_appState != AppState::Thinking) {
    return;
  }

  if (millis() - _thinkingStartMs > kThinkingTimeoutMs) {
    Serial.println("[Loop] Thinking timeout");
    _turnComplete = false;
    _turnHasAudio = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processTextReveal() {
  const int targetLen = static_cast<int>(_toolTextRevealLayout.length());
  if (_toolTextRevealIndex >= targetLen) {
    return;
  }

  const bool canReveal =
      _appRegion == AppRegion::Chat &&
      (_appState == AppState::Ready || _appState == AppState::Thinking ||
       _appState == AppState::Playing);
  if (!canReveal) {
    completeToolTextReveal();
    return;
  }

  const unsigned long now = millis();
  if (_lastTextRevealMs != 0 &&
      now - _lastTextRevealMs < kTextRevealFrameMs) {
    return;
  }

  _lastTextRevealMs = now;
  _toolTextRevealIndex++;
  _toolText = _toolTextRevealLayout.substring(0, _toolTextRevealIndex);
  _screenDirty = true;
}

void AppController::processPower() {
  if (_appRegion != AppRegion::Menu && _appState == AppState::Ready) {
    _powerManager.update();
  }
}

void AppController::processCaptivePortal() {
  if (!_wifi.isCaptivePortalActive()) {
    return;
  }

  String ssid;
  if (!_wifi.consumeProvisioningSuccess(ssid)) {
    return;
  }

  setToolTextImmediate("Saved WiFi\n" + ssid + "\nReconnecting...");
  connectNetworkStack();
}

void AppController::renderIfNeeded() {
  if (!_screenDirty) {
    return;
  }

  _screenDirty = false;
  _display.render(buildDisplayState());
}

DisplayState AppController::buildDisplayState() const {
  DisplayState state;
  state.appState = _appState;

  if (_appState == AppState::Alarm) {
    state.alarmActive = true;
    state.alarmTitle = _alarmTitle;
    state.alarmDetail = _alarmDetail;
    return state;
  }

  const bool homeMenuVisible =
      _appRegion == AppRegion::Menu && _menuState == MenuState::Home;
  if (homeMenuVisible) {
    state.headerLeft = currentTimeString();
    const int battery = M5.Power.getBatteryLevel();
    if (battery >= 0 && battery <= 100) {
      state.headerRight = String(battery) + "%";
    }
  }

  state.bodyText = buildBodyText();
  state.bodyDim = _appState == AppState::Recording ||
                  _appState == AppState::Thinking;
  state.imagePresent = _imagePresent;
  state.pageIndex = _bodyPageIndex;
  state.pageCount = currentBodyPageCount();
  state.showMenu = _appRegion == AppRegion::Menu;
  if (state.showMenu) {
    const int count = menuItemCount();
    const int visibleCount = min(MAX_MENU_VISIBLE_ITEMS, count);
    const int pageStart =
        count <= MAX_MENU_VISIBLE_ITEMS
            ? 0
            : min(_menuSelection, count - MAX_MENU_VISIBLE_ITEMS);
    state.menuItemCount = visibleCount;
    state.menuSelectedIndex = _menuSelection - pageStart;
    state.menuHasMoreAbove = pageStart > 0;
    state.menuHasMoreBelow = pageStart + visibleCount < count;
    for (int i = 0; i < visibleCount; i++) {
      state.menuItems[i] = menuItemLabel(pageStart + i);
    }
  }
  return state;
}

String AppController::buildBodyText() const {
  if (!_toolText.isEmpty()) {
    return _toolText;
  }

  // When the model has shown an image but no text, suppress the default
  // greeting/status copy so the image stands alone as a single page.
  if (_imagePresent) {
    return "";
  }

  if (_wifi.isCaptivePortalActive()) {
    return "To set up a new WiFi network, connect your phone to the WiFi "
           "network named " +
           _wifi.captivePortalSsid() +
           " and enter your network's name and password.";
  }

  switch (_appState) {
  case AppState::Connecting:
    if (_startupChecklistVisible) {
      return buildStartupChecklistText();
    }
    return _statusText.isEmpty() ? "Starting..." : _statusText;

  case AppState::Ready:
    return "Hi, how can I help? Hold the big button and speak to get a "
           "response.";

  case AppState::Recording:
    return "Hi, how can I help? Hold the big button and speak to get a "
           "response.";

  case AppState::Thinking:
    return "Thinking...";

  case AppState::Playing:
    return "";

  case AppState::ConfirmReset:
    return "Are you sure? Reset will remove data and restart into the last "
           "working version. WiFi credentials are kept.";

  case AppState::Error:
    switch (_errorCategory) {
    case ErrorCategory::Startup:
      return "Could not start device.";
    case ErrorCategory::WiFiTimeout:
      return "Could not connect to the internet.";
    case ErrorCategory::ServerRefused:
    case ErrorCategory::GeminiUnavailable:
    case ErrorCategory::None:
    default:
      return "Sorry, that didn't work.";
    }

  case AppState::Alarm:
    // Body text is unused — drawAlarm uses alarmTitle/alarmDetail directly.
    return "";
  }

  return "";
}

String AppController::buildStartupChecklistText() const {
  auto line = [](bool done, const char *label) {
    return String(done ? "[x] " : "[ ] ") + label;
  };

  return String("Starting...\n") + line(_startupPowerDone, "Powering on") +
         "\n" + line(_startupWifiDone, "WiFi") + "\n" +
         line(_startupInternetDone, "Internet");
}

int AppController::currentBodyPageCount() const {
  const String body = buildBodyText();
  const int textPages = body.isEmpty() ? 0 : _display.pageCountForText(body);
  const int total = (_imagePresent ? 1 : 0) + textPages;
  return max(1, total);
}

String AppController::currentTimeString() const {
  time_t now = time(nullptr);
  struct tm local;
  if (!localtime_r(&now, &local)) {
    return "";
  }
  if (local.tm_year + 1900 < 2024) {
    return "";
  }
  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  const char *suffix = local.tm_hour < 12 ? "AM" : "PM";
  char buf[10];
  snprintf(buf, sizeof(buf), "%d:%02d%s", hour12, local.tm_min, suffix);
  return String(buf);
}

String AppController::deviceStatusJson() const {
  JsonDocument status;
  status["firmware_version"] = FIRMWARE_VERSION;
  status["battery_percent"] = M5.Power.getBatteryLevel();
  status["volume"] = _audio.volume();
  status["brightness"] = M5.Display.getBrightness();
  status["speaker"] = _audio.useExternalSpeaker() ? "external" : "internal";
  status["external_speaker_gain"] = _audio.externalSpeakerGain();
  status["voice"] = _settings.voice();
  status["wifi_network"] = _wifi.isConnected() ? _wifi.ssid() : "disconnected";
  status["uptime_seconds"] = millis() / 1000;
  status["cpu_mhz"] = getCpuFrequencyMhz();
  status["power_timeouts"]["dim_ms"] = _powerManager.timeouts().dimMs;
  status["power_timeouts"]["screen_off_ms"] =
      _powerManager.timeouts().screenOffMs;
  status["power_timeouts"]["power_off_ms"] =
      _powerManager.timeouts().powerOffMs;

  String json;
  serializeJson(status, json);
  return json;
}

void AppController::beginFactoryReset() {
  Serial.println("[Reset] Clearing device preferences");
  _live.disconnect();
  _wifi.reset();
  _settings.reset();
  delay(100);
  ESP.restart();
}

const char *AppController::errorCategoryLabel() const {
  switch (_errorCategory) {
  case ErrorCategory::Startup:
    return "Startup";
  case ErrorCategory::WiFiTimeout:
    return "WiFi timeout";
  case ErrorCategory::ServerRefused:
    return "Server refused";
  case ErrorCategory::GeminiUnavailable:
    return "Gemini down";
  case ErrorCategory::None:
  default:
    return "Error";
  }
}

void AppController::onTimersChanged() { _screenDirty = true; }

void AppController::checkTimerExpiry() {
  TimerRecord expired[kMaxExpiredPerWake];
  const int n = _timers.harvestExpired(time(nullptr), expired, kMaxExpiredPerWake);
  if (n <= 0) {
    return;
  }

  // Detail line lists named timers only — for unnamed timers we just show the
  // bell + "ALARM".
  String namedDetail;
  for (int i = 0; i < n; i++) {
    if (expired[i].name.isEmpty()) continue;
    if (!namedDetail.isEmpty()) namedDetail += ", ";
    namedDetail += expired[i].name;
  }
  Serial.printf("[Timers] Expired count=%d names=\"%s\"\n", n, namedDetail.c_str());

  if (_appState == AppState::Alarm) {
    if (!namedDetail.isEmpty()) {
      _alarmDetail = _alarmDetail.isEmpty()
                         ? namedDetail
                         : _alarmDetail + ", " + namedDetail;
    }
    _screenDirty = true;
    return;
  }
  enterAlarmState(String("ALARM"), namedDetail);
}

void AppController::enterAlarmState(const String &title, const String &detail) {
  _alarmReturnState =
      _appState == AppState::Connecting ? AppState::Ready : _appState;
  _alarmReturnStatus = _statusText;
  _alarmTitle = title;
  _alarmDetail = detail;
  _alarmStepIndex = 0;
  _alarmStepStartMs = millis();
  _alarmStepInFlight = false;
  _audio.stopPlayback();
  if (_appState == AppState::Recording) {
    _audio.stopRecording();
    _live.sendStop();
  }
  // Bring display fully back if it had dimmed.
  _powerManager.registerActivity();
  M5.Display.setBrightness(_settings.brightness());

  _appState = AppState::Alarm;
  _appRegion = AppRegion::Chat;
  _statusText = "Timer";
  _errorText = "";
  _startupChecklistVisible = false;
  resetBodyPage();
  _screenDirty = true;
  Serial.printf("[Alarm] entered (%s)\n", detail.c_str());
}

void AppController::exitAlarmState() {
  M5.Speaker.stop();
  _alarmTitle = "";
  _alarmDetail = "";
  _alarmStepInFlight = false;
  Serial.println("[Alarm] dismissed");

  // If the alarm fired before we ever brought up the network (deep-sleep wake
  // with an expired timer), start the network stack now.
  if (!_networkStackStarted) {
    _networkStackStarted = true;
    setAppState(AppState::Connecting, "Starting...");
    renderIfNeeded();
    connectNetworkStack();
    return;
  }

  setAppState(_alarmReturnState, _alarmReturnStatus);
}

void AppController::serviceAlarmTrill() {
  if (_appState != AppState::Alarm) {
    return;
  }
  const int patternLen = alarmPatternLen();
  const AlarmStep *pattern = alarmPattern();
  const unsigned long now = millis();
  const AlarmStep &step = pattern[_alarmStepIndex];

  if (!_alarmStepInFlight) {
    if (step.freqHz > 0) {
      M5.Speaker.tone(step.freqHz, step.durationMs);
    }
    _alarmStepStartMs = now;
    _alarmStepInFlight = true;
    return;
  }

  if (now - _alarmStepStartMs >= static_cast<unsigned long>(step.durationMs)) {
    _alarmStepIndex = (_alarmStepIndex + 1) % patternLen;
    _alarmStepInFlight = false;
  }
}

bool AppController::handleAlarmButtons() {
  if (_buttonA.consumePressed() || _buttonB.consumePressed()) {
    _buttonA.clearEvents();
    _buttonB.clearEvents();
    exitAlarmState();
    return true;
  }
  return false;
}

String AppController::handleSetTimerTool(int durationSeconds,
                                         const String &name) {
  TimerRecord created;
  const TimerService::Result rc =
      _timers.addTimer(static_cast<uint32_t>(max(0, durationSeconds)), name, created);
  onTimersChanged();
  return formatTimerSummary(time(nullptr)) +
         (rc == TimerService::Result::Ok
              ? String(" | started")
              : String(" | error: ") + TimerService::describeResult(rc));
}

String AppController::handleListTimersTool() {
  const time_t now = time(nullptr);
  return _timers.describeAll(now);
}

String AppController::handleCancelTimerTool(const TimerRef &ref, bool all) {
  if (all) {
    const int n = _timers.cancelAll();
    onTimersChanged();
    return formatTimerSummary(time(nullptr)) + " | cancelled " + n + " timer(s)";
  }
  TimerRecord cancelled;
  const TimerService::Result rc = _timers.cancel(ref, cancelled);
  onTimersChanged();
  return formatTimerSummary(time(nullptr)) +
         (rc == TimerService::Result::Ok
              ? String(" | cancelled ") +
                    (cancelled.name.isEmpty() ? String("timer") : cancelled.name)
              : String(" | error: ") + TimerService::describeResult(rc));
}

String AppController::handleExtendTimerTool(int deltaSeconds,
                                            const TimerRef &ref) {
  TimerRecord adjusted;
  const TimerService::Result rc = _timers.extend(ref, deltaSeconds, adjusted);
  onTimersChanged();
  return formatTimerSummary(time(nullptr)) +
         (rc == TimerService::Result::Ok
              ? String(" | adjusted ") +
                    (adjusted.name.isEmpty() ? String("timer") : adjusted.name)
              : String(" | error: ") + TimerService::describeResult(rc));
}

String AppController::formatTimerSummary(time_t now) const {
  return _timers.describeAll(now);
}
