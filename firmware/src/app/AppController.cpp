#include "AppController.h"

#include "../Config.h"
#include "../diag/Log.h"
#include "../hal/Board.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <time.h>

void AppController::setup() {
  Serial.begin(115200);
  Log::setSink(&AppController::bootLogTrampoline, this);
  Log::client("Boot", "Waveshare AMOLED Live Voice Assistant");
  Serial.flush();

  Board::init();
  setCpuFrequencyMhz(240);
  Log::client("Setup", "CPU clock set to %lu MHz", getCpuFrequencyMhz());

  setenv("TZ", LOCAL_TZ, 1);
  tzset();

  _settings.init();

  _display.init();
  _display.setBrightness(_settings.brightness());
  _displayReady = true;
  _screenDirty = true;
  renderIfNeeded();

  _powerManager.setSavedBrightness(_settings.brightness());
  configureCallbacks();

  _wifi.init();

  if (!_audio.init()) {
    setErrorState(ErrorCategory::Startup, "Startup failed",
                  "Audio buffer unavailable");
    renderIfNeeded();
    return;
  }

  _audio.setVolume(_settings.volume());
  _chatId = _settings.chatId();
  _live.setChatId(_chatId);
  _live.setVoice(_settings.voice());
  _live.setPreferredEndpointIndex(_settings.serverEndpointIndex());

  LiveSessionCallbacks callbacks;
  callbacks.onActivity = [this]() { _powerManager.registerActivity(); };
  callbacks.onStatus = [this](const String &status) {
    if (_appState != AppState::Error) {
      setAppState(AppState::Connecting, status);
    }
  };
  callbacks.onReady = [this]() {
    exitBootMode();
    _appRegion = AppRegion::Chat;
    setAppState(AppState::Ready, "Ready");
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
    _settings.setChatId(chatId);
    _live.setChatId(chatId);
    _screenDirty = true;
  };
  callbacks.onConversationReset = [this]() {
    _audio.stopPlayback();
    _toolText = "";
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
    _toolText = text;
    resetBodyPage();
    _screenDirty = true;
  };
  auto applyPendingTurnReset = [this]() {
    if (!_pendingTurnReset) {
      return;
    }
    _toolText = "";
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
    const String delta = transcriptDelta(_toolText, text);
    if (delta.isEmpty()) {
      return;
    }
    _toolText += delta;
    resetBodyPage();
    _screenDirty = true;
  };
  callbacks.onError = [this](const String &category, const String &error) {
    const ErrorCategory mapped = category == "gemini_unavailable"
                                     ? ErrorCategory::GeminiUnavailable
                                     : ErrorCategory::ServerRefused;
    setErrorState(mapped, "Server error", error);
  };
  callbacks.onIgnoredAudio = [this](const String &reason) {
    Log::client("Recording",
                "ignored by server reason=%s chunks=%d failed=%d bytes=%u "
                "capture_failures=%d",
                reason.c_str(), _audioChunksSent, _audioChunksFailed,
                static_cast<unsigned>(_audioBytesSent), _captureFailures);
    _turnComplete = false;
    _turnHasAudio = false;
    _pendingTurnReset = false;
    _audio.stopPlayback();
    setDebugText(String("DBG ignored:") +
                 (reason.isEmpty() ? String("unknown") : reason));
    setAppState(AppState::Ready, "Ready");
  };
  callbacks.onAudio = [this](const uint8_t *data, size_t len) {
    if (_appState != AppState::Recording) {
      clearDebugText();
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
  callbacks.onPlaySound = [this](const String &sound) {
    _powerManager.registerActivity();
    return _audio.playNamedSound(sound);
  };
  callbacks.onPlayMelody = [this](const String &notes) {
    _powerManager.registerActivity();
    return _audio.playMelody(notes);
  };
  callbacks.onPowerOff = [this]() { performPowerOff(); };
  callbacks.onPowerTimeouts =
      [this](unsigned long dimMs, unsigned long screenOffMs,
             unsigned long lightSleepMs, unsigned long powerOffMs) {
        _powerManager.setTimeouts({.dimMs = dimMs,
                                   .screenOffMs = screenOffMs,
                                   .lightSleepMs = lightSleepMs,
                                   .powerOffMs = powerOffMs});
      };
  callbacks.getDeviceStatusJson = [this]() { return deviceStatusJson(); };
  callbacks.onVoiceChanged = [this](const String &voice) {
    _settings.setVoice(voice);
  };
  callbacks.onEndpointIndexChanged = [this](int endpointIndex) {
    _settings.setServerEndpointIndex(endpointIndex);
  };

  _live.init(callbacks);

  Log::client("Audio", "capture=%d Hz chunk=%d ms bytes=%u",
              MIC_SAMPLE_RATE, MIC_CHUNK_MS,
              static_cast<unsigned>(_audio.captureBytes()));
  Log::client("Audio", "playback=%d Hz max=%d s", PLAY_SAMPLE_RATE,
              MAX_PLAYBACK_SEC);

  renderIfNeeded();

  connectNetworkStack();
  renderIfNeeded();
}

void AppController::loop() {
  Board::update();

  processPlayback();

  const bool audioActive = _appState == AppState::Playing;

  if (!audioActive && millis() - _lastPowerPollMs > 3000) {
    _lastPowerPollMs = millis();
    const uint16_t vbat = Board::batteryVoltageMv();
    const uint16_t vin = Board::vbusVoltageMv();
    const int level = Board::batteryLevel();
    Log::client("Power", "vbat=%u mV level=%d vbus=%u src=%s heap=%uK",
                vbat, level, vin, Board::powerSourceLabel(),
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  }

  if (!audioActive && millis() - _lastHeartbeatMs > 3000) {
    _lastHeartbeatMs = millis();
    Log::client("Loop", "state=%d region=%d power=%s ws=%d sleep=%d",
                static_cast<int>(_appState), static_cast<int>(_appRegion),
                powerStateName(_powerManager.getState()), _live.isConnected(),
                _powerManager.getState() == PowerState::LightSleep);
  }

  if (!audioActive && millis() - _lastHeaderRefreshMs > 30000) {
    _lastHeaderRefreshMs = millis();
    _screenDirty = true;
  }

  if (!audioActive) {
    _wifi.poll();
  }
  _live.poll();
  processPlayback();
  if (_appState != AppState::Playing) {
    _live.reconnectIfNeeded(_wifi.isConnected() &&
                            _powerManager.getState() !=
                                PowerState::LightSleep &&
                            _appState != AppState::Error);
  }

  handleButtons();
  processRecording();
  processThinkingTimeout();
  processMenuFetches();
  processPower();
  processCaptivePortal();
  renderIfNeeded();
  if (_appState != AppState::Playing) {
    delay(1);
  }
}

void AppController::configureCallbacks() {
  _powerManager.onBrightnessChange(
      [this](int brightness) { _display.setBrightness(brightness); });

  _powerManager.onWiFiStateChange(
      [this](bool enabled) { setNetworkEnabled(enabled); });

  _powerManager.onPowerOff([this]() { performPowerOff(); });
}

void AppController::connectNetworkStack() {
  _appRegion = AppRegion::Chat;
  setAppState(AppState::Connecting, "Connecting...");
  if (!_wifi.connectKnownNetworks()) {
    setErrorState(ErrorCategory::WiFiTimeout, "WiFi failed",
                  "A retry  B hold menu");
    return;
  }

  restoreSessionPreview();
  _screenDirty = true;
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
  Log::client("Power", "powering off");
  _live.disconnect();
  _wifi.disconnect();
  delay(100);
  Board::powerOff();
}

void AppController::clearToolText() {
  _toolText = "";
  if (_imagePresent) {
    _display.clearImage();
    _imagePresent = false;
  }
  resetBodyPage();
}

void AppController::setDebugText(const String &text) {
  if (_debugText == text) {
    return;
  }
  _debugText = text;
  _screenDirty = true;
}

void AppController::clearDebugText() {
  if (_debugText.isEmpty()) {
    return;
  }
  _debugText = "";
  _screenDirty = true;
}

void AppController::resetBodyPage() { _bodyPageIndex = 0; }

String AppController::transcriptDelta(const String &current,
                                      const String &incoming) const {
  if (incoming.isEmpty()) {
    return "";
  }
  if (current.isEmpty()) {
    return incoming;
  }

  // Gemini Live transcription usually streams deltas, but early partial
  // hypotheses can repeat as cumulative or overlapping text. Keep the on-screen
  // text append-only so repeated partials do not duplicate words.
  if (incoming == current || current.startsWith(incoming)) {
    return "";
  }
  if (incoming.startsWith(current)) {
    return incoming.substring(current.length());
  }
  if (current.endsWith(incoming)) {
    return "";
  }

  const int maxOverlap = min(current.length(), incoming.length());
  for (int len = maxOverlap; len >= 1; len--) {
    if (current.endsWith(incoming.substring(0, len))) {
      return incoming.substring(len);
    }
  }

  return incoming;
}

void AppController::bootLogTrampoline(void *ctx, char side, const char *topic,
                                      const char *message) {
  (void)side;
  static_cast<AppController *>(ctx)->appendBootLog(topic, message);
}

void AppController::appendBootLog(const char *topic, const char *message) {
  if (!_bootMode) {
    return;
  }

  String entry;
  entry.reserve(strlen(topic) + 2 + strlen(message));
  entry += topic;
  entry += ": ";
  entry += message;

  if (!_bootLog.isEmpty()) {
    _bootLog += '\n';
  }
  _bootLog += entry;

  // Drop oldest entries until the wrapped log fits on one body page.
  while (_displayReady &&
         _display.wrappedRowCount(_bootLog) > TextDisplay::kChatRows) {
    const int cut = _bootLog.indexOf('\n');
    if (cut < 0) {
      break;
    }
    _bootLog = _bootLog.substring(cut + 1);
  }

  _screenDirty = true;
  renderIfNeeded();
}

void AppController::exitBootMode() {
  if (!_bootMode) {
    return;
  }
  _bootMode = false;
  _bootLog = "";
  Log::clearSink();
  _screenDirty = true;
}

void AppController::restoreSessionPreview() {
  if (_chatId.isEmpty()) {
    return;
  }

  String lastMessage;
  if (_live.fetchLastAssistantMessage(lastMessage) && !lastMessage.isEmpty()) {
    _toolText = lastMessage;
    _statusText = "Restored";
    resetBodyPage();
    _screenDirty = true;
  }
}

void AppController::clearButtonEvents() {
  _buttonA.clearEvents();
  _buttonB.clearEvents();
}

void AppController::handleButtons() {
  const unsigned long now = millis();
  _buttonA.update(Board::buttonAIsPressed(), now);
  _buttonB.update(Board::buttonBIsPressed(), now);

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
    openMenu(_appState == AppState::Error ? MenuState::Device : MenuState::Home);
    return;
  }

  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
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
  if (_appState == AppState::Recording) {
    _audio.stopRecording();
    if (_live.isConnected()) {
      _live.sendStop();
    }
    _pendingTurnReset = false;
    setAppState(AppState::Ready, "Ready");
  }
  _appRegion = AppRegion::Menu;
  _menuState = state;
  _menuSelection = 0;
  clearButtonEvents();
  if (state == MenuState::ResumeChat) {
    startConversationHistoryLoad();
  } else if (state == MenuState::Updates) {
    startFirmwareUpdateCheck();
  }
  _screenDirty = true;
}

void AppController::closeMenu() {
  _appRegion = AppRegion::Chat;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  clearButtonEvents();
  _screenDirty = true;
}

void AppController::navigateBackFromMenu() {
  if (_menuState == MenuState::Home) {
    closeMenu();
    return;
  }

  if (_menuState == MenuState::Updates) {
    openMenu(MenuState::Device);
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
      openMenu(MenuState::Updates);
      return;
    case 3:
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
    if (_historyLoadStatus == MenuLoadStatus::Loading) {
      return;
    }
    if (_historyCount == 0) {
      return;
    }
    resumeConversation(_menuSelection - 1);
    return;

  case MenuState::Updates:
    if (_menuSelection == 0) {
      openMenu(MenuState::Device);
      return;
    }
    if (_firmwareCheckStatus == MenuLoadStatus::Loading ||
        _firmwareCheckStatus == MenuLoadStatus::Idle) {
      return;
    }
    if (_menuSelection == 1 && _firmwareCheckStatus == MenuLoadStatus::Loaded &&
        _firmwareInfo.available && !_firmwareInfo.downloadUrl.isEmpty()) {
      installFirmwareUpdate();
    }
    return;
  }
}

int AppController::menuItemCount() const {
  switch (_menuState) {
  case MenuState::Home:
    return 4;
  case MenuState::Device:
    return 4;
  case MenuState::ResumeChat:
    return _historyCount > 0 ? 1 + _historyCount : 2;
  case MenuState::Updates:
    if (_firmwareCheckStatus == MenuLoadStatus::Loaded &&
        _firmwareInfo.available && !_firmwareInfo.notes.isEmpty()) {
      return 3;
    }
    return 2;
  }
  return 0;
}

String AppController::menuTitle() const {
  switch (_menuState) {
  case MenuState::Home:
    return "Menu";
  case MenuState::Device:
    return "Device";
  case MenuState::ResumeChat:
    return "Resume";
  case MenuState::Updates:
    return "Updates";
  }
  return "";
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
      return "Turn off";
    default:
      return "";
    }

  case MenuState::ResumeChat:
    if (index == 0) {
      return "Go back";
    }
    if (_historyLoadStatus == MenuLoadStatus::Loading ||
        _historyLoadStatus == MenuLoadStatus::Idle) {
      return "Loading...";
    }
    if (_historyCount == 0 && index == 1) {
      return _historyLoadMessage.isEmpty() ? String("No saved chats")
                                           : _historyLoadMessage.substring(0, 26);
    }
    if (index - 1 < _historyCount) {
      const ConversationSummary &entry = _history[index - 1];
      const String preview =
          entry.lastMessage.isEmpty() ? entry.chatId : entry.lastMessage;
      return preview.substring(0, 26);
    }
    return "";

  case MenuState::Updates:
    if (index == 0) {
      return "Go back";
    }
    if (_firmwareCheckStatus == MenuLoadStatus::Loading ||
        _firmwareCheckStatus == MenuLoadStatus::Idle) {
      return "Checking...";
    }
    if (_firmwareCheckStatus == MenuLoadStatus::Failed) {
      return _firmwareCheckMessage.isEmpty()
                 ? String("Update check failed")
                 : _firmwareCheckMessage.substring(0, 26);
    }
    if (!_firmwareInfo.available) {
      return "Up to date v" + String(FIRMWARE_VERSION);
    }
    if (_firmwareInfo.downloadUrl.isEmpty()) {
      return "Update unavailable";
    }
    if (index == 1) {
      return "Install v" + String(_firmwareInfo.latestVersion);
    }
    return _firmwareInfo.notes.substring(0, 26);
  }

  return "";
}

void AppController::startConversationHistoryLoad() {
  _historyCount = 0;
  _historyLoadMessage = "";
  _historyLoadStatus = MenuLoadStatus::Loading;

  if (_historyFetchTask != nullptr) {
    return;
  }

  if (!_wifi.isConnected()) {
    _historyLoadStatus = MenuLoadStatus::Failed;
    _historyLoadMessage = "Connect WiFi first";
    return;
  }

  _historyFetchDone = false;
  _historyFetchOk = false;
  _historyFetchCount = 0;
  _historyFetchMessage = "";

  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AppController::conversationHistoryLoadTaskTrampoline,
                              "menu_history", kMenuFetchTaskStack, this, 1,
                              &_historyFetchTask, 1);
  if (ok != pdPASS) {
    _historyFetchTask = nullptr;
    _historyLoadStatus = MenuLoadStatus::Failed;
    _historyLoadMessage = "History unavailable";
    Log::client("Menu", "failed to start history fetch task");
  }
}

void AppController::conversationHistoryLoadTaskTrampoline(void *ctx) {
  static_cast<AppController *>(ctx)->conversationHistoryLoadTask();
}

void AppController::conversationHistoryLoadTask() {
  int count = 0;
  _historyFetchOk = _live.fetchConversationHistory(
      _historyFetchResults, kMaxConversationHistory, count);
  _historyFetchCount = count;
  if (!_historyFetchOk) {
    _historyFetchMessage = "History unavailable";
  } else if (count == 0) {
    _historyFetchMessage = "No saved chats";
  } else {
    _historyFetchMessage = "";
  }
  _historyFetchDone = true;
  vTaskDelete(nullptr);
}

void AppController::finishConversationHistoryLoad() {
  _historyFetchTask = nullptr;
  _historyCount = 0;
  if (_historyFetchOk) {
    _historyLoadStatus = MenuLoadStatus::Loaded;
    _historyCount = min(_historyFetchCount, kMaxConversationHistory);
    for (int i = 0; i < _historyCount; i++) {
      _history[i] = _historyFetchResults[i];
    }
  } else {
    _historyLoadStatus = MenuLoadStatus::Failed;
  }

  _historyLoadMessage = _historyFetchMessage;
  if (_historyLoadMessage.isEmpty() && _historyCount == 0) {
    _historyLoadMessage = "No saved chats";
  }
  _screenDirty = true;
}

void AppController::resumeConversation(int index) {
  if (index < 0 || index >= _historyCount) {
    return;
  }

  const ConversationSummary &entry = _history[index];
  _chatId = entry.chatId;
  _settings.setChatId(_chatId);
  _live.setChatId(_chatId);
  _toolText = entry.lastMessage;
  if (_imagePresent) {
    _display.clearImage();
    _imagePresent = false;
  }
  resetBodyPage();
  closeMenu();
  _live.disconnect();
  setAppState(AppState::Connecting, "Restoring...");
  _live.connect();
}

void AppController::startFreshConversation() {
  _chatId = "";
  _settings.clearChatId();
  _live.setChatId("");
  clearToolText();
  _live.disconnect();
  setAppState(AppState::Connecting, "New chat...");
  _live.connect();
}

void AppController::startCaptivePortalFlow() {
  _live.disconnect();
  if (_wifi.startCaptivePortal()) {
    setAppState(AppState::Connecting, "WiFi setup");
    _toolText = "Join AP\n" + _wifi.captivePortalSsid() + "\nOpen " +
                _wifi.captivePortalIp() + "\nSubmit WiFi form";
  } else {
    setErrorState(ErrorCategory::WiFiTimeout, "Portal failed",
                  "Could not start setup AP");
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::startFirmwareUpdateCheck() {
  _firmwareInfo = FirmwareUpdateInfo{};
  _firmwareCheckMessage = "";
  _firmwareCheckStatus = MenuLoadStatus::Loading;

  if (_firmwareFetchTask != nullptr) {
    return;
  }

  if (!_wifi.isConnected()) {
    _firmwareCheckStatus = MenuLoadStatus::Failed;
    _firmwareCheckMessage = "Offline";
    return;
  }

  _firmwareFetchDone = false;
  _firmwareFetchOk = false;
  _firmwareFetchInfo = FirmwareUpdateInfo{};
  _firmwareFetchMessage = "";

  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AppController::firmwareUpdateCheckTaskTrampoline,
                              "menu_update", kMenuFetchTaskStack, this, 1,
                              &_firmwareFetchTask, 1);
  if (ok != pdPASS) {
    _firmwareFetchTask = nullptr;
    _firmwareCheckStatus = MenuLoadStatus::Failed;
    _firmwareCheckMessage = "Update check failed";
    Log::client("Menu", "failed to start firmware check task");
  }
}

void AppController::firmwareUpdateCheckTaskTrampoline(void *ctx) {
  static_cast<AppController *>(ctx)->firmwareUpdateCheckTask();
}

void AppController::firmwareUpdateCheckTask() {
  _firmwareFetchInfo = FirmwareUpdateInfo{};
  _firmwareFetchOk = _live.checkFirmwareUpdate(_firmwareFetchInfo);
  if (!_firmwareFetchOk) {
    _firmwareFetchMessage = "Update check failed";
  } else if (_firmwareFetchInfo.available &&
             _firmwareFetchInfo.downloadUrl.isEmpty()) {
    _firmwareFetchMessage = "No download URL";
  } else {
    _firmwareFetchMessage = "";
  }
  _firmwareFetchDone = true;
  vTaskDelete(nullptr);
}

void AppController::finishFirmwareUpdateCheck() {
  _firmwareFetchTask = nullptr;
  if (_firmwareFetchOk) {
    _firmwareCheckStatus = MenuLoadStatus::Loaded;
    _firmwareInfo = _firmwareFetchInfo;
  } else {
    _firmwareCheckStatus = MenuLoadStatus::Failed;
    _firmwareInfo = FirmwareUpdateInfo{};
  }
  _firmwareCheckMessage = _firmwareFetchMessage;
  _screenDirty = true;
}

void AppController::installFirmwareUpdate() {
  if (!_firmwareInfo.available || _firmwareInfo.downloadUrl.isEmpty()) {
    return;
  }

  const FirmwareUpdateInfo info = _firmwareInfo;
  closeMenu();
  setAppState(AppState::Connecting, "Updating...");
  _toolText =
      "Downloading update\nv" + String(info.latestVersion) + "\nPlease wait";
  resetBodyPage();
  _screenDirty = true;
  renderIfNeeded();

  String error;
  if (_live.downloadAndApplyFirmwareUpdate(info.downloadUrl, error)) {
    _toolText = "Update installed\nRestarting...";
    resetBodyPage();
    _screenDirty = true;
    renderIfNeeded();
    delay(500);
    ESP.restart();
  }

  _toolText = "Update failed\n" + error;
  setAppState(AppState::Ready, "Ready");
  _screenDirty = true;
}

void AppController::startRecording() {
  Log::client("Recording", "start ws=%d state=%d", _live.isConnected(),
              static_cast<int>(_appState));
  if (_appRegion == AppRegion::Menu) {
    Log::client("Recording", "start rejected: menu open");
    return;
  }
  _powerManager.registerActivity();
  _audio.stopPlayback();
  clearDebugText();
  if (!_live.isConnected()) {
    Log::client("Recording", "start rejected: websocket disconnected");
    setDebugText("DBG websocket offline");
    setAppState(AppState::Ready, "Ready");
    return;
  }
  if (!_audio.startRecording()) {
    Log::client("Recording", "audio start failed");
    setDebugText("DBG mic start failed");
    setAppState(AppState::Ready, "Ready");
    return;
  }
  _turnComplete = false;
  _turnHasAudio = false;
  _audioChunksSent = 0;
  _audioChunksFailed = 0;
  _captureFailures = 0;
  _audioBytesSent = 0;
  _lastCaptureFailureLogMs = 0;
  _recordingStartMs = millis();
  if (!_live.sendStart()) {
    Log::client("Recording", "start send failed");
    _audio.stopRecording();
    setDebugText("DBG start send fail");
    setAppState(AppState::Ready, "Ready");
    return;
  }
  setAppState(AppState::Recording, "Listening...");
  renderIfNeeded();
}

void AppController::stopRecording() {
  const unsigned long durationMs = millis() - _recordingStartMs;
  Log::client("Recording",
              "stop duration=%lu sent=%d failed=%d bytes=%u "
              "capture_failures=%d",
              durationMs, _audioChunksSent, _audioChunksFailed,
              static_cast<unsigned>(_audioBytesSent), _captureFailures);
  _audio.stopRecording();
  if (_audioChunksSent == 0) {
    Log::client("Recording", "no audio chunks sent before stop");
    setDebugText("DBG no audio sent");
  }
  if (!_live.sendStop()) {
    Log::client("Recording", "stop send failed");
    setDebugText("DBG stop send fail");
    _pendingTurnReset = false;
    setAppState(AppState::Ready, "Ready");
    return;
  }
  _pendingTurnReset = true;
  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Thinking...");
}

void AppController::processRecording() {
  if (_appState != AppState::Recording) {
    return;
  }

  if (_appRegion == AppRegion::Menu) {
    Log::client("Recording", "stopping capture because menu opened");
    _audio.stopRecording();
    if (_live.isConnected()) {
      _live.sendStop();
    }
    _pendingTurnReset = false;
    setAppState(AppState::Ready, "Ready");
    return;
  }

  if (millis() - _recordingStartMs >= kMaxRecordingMs) {
    Log::client("Recording", "max recording time reached");
    stopRecording();
    return;
  }

  if (!_audio.captureChunk()) {
    _captureFailures++;
    const unsigned long now = millis();
    if (_captureFailures <= 3 ||
        now - _lastCaptureFailureLogMs >= kCaptureFailureLogIntervalMs) {
      _lastCaptureFailureLogMs = now;
      Log::client("Recording", "mic capture returned false count=%d",
                  _captureFailures);
    }
    if (_captureFailures == 1) {
      setDebugText("DBG mic read false");
    }
    return;
  }

  const bool sent =
      _live.sendAudio(_audio.captureData(), _audio.captureBytes());
  _powerManager.registerActivity();
  if (!sent) {
    _audioChunksFailed++;
    Log::client("Recording", "audio chunk send failed count=%d ws=%d",
                _audioChunksFailed, _live.isConnected());
    setDebugText("DBG audio send fail");
    return;
  }

  _audioChunksSent++;
  _audioBytesSent += _audio.captureBytes();
  if (_audioChunksSent <= 5 || _audioChunksSent % 10 == 0) {
    Log::client("Recording",
                "#%d sent bytes=%u total=%u avg_abs=%d peak=%d ch=%d "
                "l_avg=%d r_avg=%d",
                _audioChunksSent,
                static_cast<unsigned>(_audio.captureBytes()),
                static_cast<unsigned>(_audioBytesSent),
                _audio.lastCaptureAverageAbs(), _audio.lastCapturePeak(),
                _audio.lastCaptureChannel(), _audio.lastCaptureLeftAverageAbs(),
                _audio.lastCaptureRightAverageAbs());
  }
}

void AppController::processPlayback() {
  if (_appState != AppState::Thinking && _appState != AppState::Playing) {
    return;
  }

  const int buffered = _audio.bufferedPlaybackBytes();
  const bool hasEnoughBuffered = buffered >= kMinPlaybackBytes;
  const bool hasCompleteShortReply = _turnComplete && buffered > 0;
  if (!_audio.playbackStarted() &&
      (hasEnoughBuffered || hasCompleteShortReply)) {
    _audio.markPlaybackStarted();
    setAppState(AppState::Playing, "Speaking...");
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
    Log::client("Loop",
                "thinking timeout chunks=%d failed=%d bytes=%u "
                "has_audio=%d turn_complete=%d buffered=%d",
                _audioChunksSent, _audioChunksFailed,
                static_cast<unsigned>(_audioBytesSent), _turnHasAudio,
                _turnComplete, _audio.bufferedPlaybackBytes());
    _turnComplete = false;
    _turnHasAudio = false;
    _audio.stopPlayback();
    setDebugText("DBG response timeout");
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processMenuFetches() {
  if (_historyFetchTask != nullptr && _historyFetchDone) {
    finishConversationHistoryLoad();
  }

  if (_firmwareFetchTask != nullptr && _firmwareFetchDone) {
    finishFirmwareUpdateCheck();
  }
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

  _toolText = "Saved WiFi\n" + ssid + "\nReconnecting...";
  resetBodyPage();
  _screenDirty = true;
  connectNetworkStack();
}

void AppController::renderIfNeeded() {
  if (!_screenDirty || _renderInProgress || !_displayReady) {
    return;
  }

  const bool audioActive = _appState == AppState::Playing ||
                           _audio.playbackStarted() ||
                           _audio.bufferedPlaybackBytes() > 0 ||
                           _audio.speakerBusy();
  if (audioActive) {
    return;
  }

  _renderInProgress = true;
  _screenDirty = false;
  _display.render(buildDisplayState());
  _renderInProgress = false;
}

DisplayState AppController::buildDisplayState() const {
  DisplayState state;
  state.appState = _appState;

  if (_appRegion == AppRegion::Menu) {
    state.headerLeft = "< Back";
    state.headerRight = menuTitle();
  }

  state.bodyText = buildBodyText();
  state.bodyDim = _appState == AppState::Recording ||
                  (_appState == AppState::Thinking && _toolText.isEmpty());
  state.imagePresent = _imagePresent;
  state.pageIndex = _bodyPageIndex;
  state.pageCount = currentBodyPageCount();
  if (!_debugText.isEmpty() && _appRegion == AppRegion::Chat) {
    state.footerLeft = _debugText;
    if (state.pageCount > 1) {
      state.footerRight =
          String(constrain(state.pageIndex + 1, 1, state.pageCount)) + "/" +
          String(state.pageCount);
    }
  }
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
  if (_bootMode) {
    return _bootLog.isEmpty() ? String("Starting...") : _bootLog;
  }

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
    return "Starting...";

  case AppState::Ready:
    return "Hi, how can I help? Hold the button and speak to get a response.";

  case AppState::Recording:
    return "Hi, how can I help? Hold the button and speak to get a response.";

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
  }

  return "";
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
  status["battery_percent"] = Board::batteryLevel();
  status["battery_voltage_mv"] = Board::batteryVoltageMv();
  status["vbus_voltage_mv"] = Board::vbusVoltageMv();
  status["power_source"] = Board::powerSourceLabel();
  status["volume"] = _audio.volume();
  status["brightness"] = Board::displayBrightness();
  status["voice"] = _settings.voice();
  status["server_endpoint"] = _live.activeEndpointLabel();
  status["wifi_network"] = _wifi.isConnected() ? _wifi.ssid() : "disconnected";
  status["uptime_seconds"] = millis() / 1000;
  status["power_timeouts"]["dim_ms"] = _powerManager.timeouts().dimMs;
  status["power_timeouts"]["screen_off_ms"] =
      _powerManager.timeouts().screenOffMs;
  status["power_timeouts"]["light_sleep_ms"] =
      _powerManager.timeouts().lightSleepMs;
  status["power_timeouts"]["power_off_ms"] =
      _powerManager.timeouts().powerOffMs;

  String json;
  serializeJson(status, json);
  return json;
}

void AppController::beginFactoryReset() {
  Log::client("Reset", "clearing device preferences");
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
