#include "AppController.h"

#include "../Config.h"
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>

void AppController::setup() {
  const unsigned long bootStartMs = millis();
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== M5 Live Voice Assistant ===");
  Serial.flush();

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_imu = true;
  M5.begin(cfg);
  M5.Power.setChargeCurrent(CHARGE_CURRENT_MA);
  updateBatteryStatus(true);

  setCpuFrequencyMhz(80);
  Serial.printf("[Setup] CPU clock set to %lu MHz\n", getCpuFrequencyMhz());

  setenv("TZ", LOCAL_TZ, 1);
  tzset();

  _settings.init();

  _display.init();
  _display.setBrightness(_settings.brightness());

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
  _voiceMode = _settings.voiceMode();
  _live.setChatId(_chatId);
  _live.setVoiceMode(_voiceMode);

  LiveSessionCallbacks callbacks;
  callbacks.onActivity = [this]() { _powerManager.registerActivity(); };
  callbacks.onStatus = [this](const String &status) {
    if (_appState != AppState::Error) {
      setAppState(AppState::Connecting, status);
    }
  };
  callbacks.onReady = [this]() {
    if (_appRegion != AppRegion::Menu && _appRegion != AppRegion::Scene) {
      _appRegion = AppRegion::Chat;
    }
    // A Gemini reconnect can complete while the user is already holding A for
    // the replacement turn after an interruption. Do not force Ready here or
    // processRecording() will stop running and the device may never send the
    // matching stop/activityEnd for that recording.
    if (_appState == AppState::Recording) {
      _screenDirty = true;
      return;
    }
    setAppState(AppState::Ready, "Ready");
    if (_scenePromptPending && _appRegion == AppRegion::Scene) {
      _scenePromptPending = false;
      promptSceneConversation();
    } else {
      maybeSendReadingAssistantIntro();
      maybeSendQuizIntro();
    }
  };
  callbacks.onTurnComplete = [this](const TurnCompleteInfo &info) {
    // Treat audio, model transcript, or tool activity as a real assistant turn.
    // Tool-only turns (for example set_volume/save_flashcard confirmations) may
    // complete without audio; ignoring those leaves the device stuck Thinking.
    if (info.hadAudio) {
      _turnHasAudio = true;
    }
    if (info.hadModelText) {
      _turnHasModelText = true;
    }
    if (info.hadToolActivity) {
      _turnHasToolActivity = true;
    }
    if (_turnHasAudio || _turnHasModelText || _turnHasToolActivity ||
        info.synthetic) {
      _turnComplete = true;
      _screenDirty = true;
    }
  };
  callbacks.onDropAudio = [this]() {
    // Gemini detected a user interrupt mid-response. Flush any queued tail of
    // the prior turn so it doesn't play into the new one. Don't change state —
    // the user is mid-recording; release will drive the next transition.
    _audio.stopPlayback();
    _turnComplete = false;
    _turnHasAudio = false;
    _turnHasModelText = false;
    _turnHasToolActivity = false;
  };
  callbacks.onChatId = [this](const String &chatId) {
    _chatId = chatId;
    _settings.setChatId(chatId);
    _live.setChatId(chatId);
    _screenDirty = true;
  };
  callbacks.onShowText = [this](const String &text) {
    _turnHasToolActivity = true;
    _toolText = text;
    resetBodyPage();
    _screenDirty = true;
  };
  callbacks.onTranscript = [this](const String &source, const String &text) {
    // User transcripts are useful for logs/server-side feedback, but do not
    // render them on-device. Keep the main screen as the assistant chat turn.
    if (source != "model") {
      return;
    }
    _turnHasModelText = true;
    _toolText += text;
    resetBodyPage();
    _screenDirty = true;
  };
  callbacks.onTurnFeedback = [this](const String &color, const String &correction,
                                    const String &reason) {
    _turnFeedbackColor = color;
    _turnFeedbackCorrection = correction;
    _turnFeedbackReason = reason;
    // Keep instant feedback completely separate from the main chat text/audio.
    // It should only update the overlay square and never interrupt the model's
    // spoken/text response path.
    _screenDirty = true;
  };
  auto applyFaceEmotion = [this](const String &emotion) {
    if (emotion == "angry" || emotion == "focused") {
      _faceEmotion = 1;
    } else if (emotion == "eepy" || emotion == "thinking") {
      _faceEmotion = 2;
    } else {
      _faceEmotion = 0;
    }
  };
  callbacks.onFaceEmotion = [this, applyFaceEmotion](const String &emotion) {
    applyFaceEmotion(emotion);
    _screenDirty = true;
  };
  callbacks.onFaceControl = [this, applyFaceEmotion](const String &emotion,
                                                     float lookX, float lookY,
                                                     float spacing, float speed) {
    applyFaceEmotion(emotion);
    _eyeLookX = constrain(lookX, -1.0f, 1.0f);
    _eyeLookY = constrain(lookY, -1.0f, 1.0f);
    _faceEyeSpacing = constrain(spacing, 36.0f, 70.0f);
    _faceAnimSpeed = constrain(speed, 0.25f, 3.0f);
    _screenDirty = true;
  };
  callbacks.onError = [this](const String &category, const String &error) {
    const ErrorCategory mapped = category == "gemini_unavailable"
                                     ? ErrorCategory::GeminiUnavailable
                                     : ErrorCategory::ServerRefused;
    setErrorState(mapped, "Server error", error);
  };
  callbacks.onIgnoredAudio = [this](const String &reason) {
    _turnComplete = false;
    _turnHasAudio = false;
    _turnHasModelText = false;
    _turnHasToolActivity = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
    _toolText =
        reason == "silent" ? "Ignored silent clip" : "Ignored short clip";
    resetBodyPage();
    _screenDirty = true;
  };
  callbacks.onAudio = [this](const uint8_t *data, size_t len) {
    if (_appState != AppState::Recording) {
      _audio.queuePlayback(data, len);
      _turnHasAudio = true;
      _screenDirty = true;
    }
  };
  callbacks.onBrightness = [this](int level) {
    _turnHasToolActivity = true;
    _display.setBrightness(level);
    _powerManager.setSavedBrightness(level);
    _settings.setBrightness(level);
    _screenDirty = true;
  };
  callbacks.onVolume = [this](int level) {
    _turnHasToolActivity = true;
    _audio.setVolume(level);
    _settings.setVolume(level);
  };
  callbacks.onPlaySound = [this](const String &sound) {
    _turnHasToolActivity = true;
    _powerManager.registerActivity();
    return _audio.playNamedSound(sound);
  };
  callbacks.onPlayMelody = [this](const String &notes) {
    _turnHasToolActivity = true;
    _powerManager.registerActivity();
    return _audio.playMelody(notes);
  };
  callbacks.onPowerOff = [this]() {
    _live.disconnect();
    _wifi.disconnect();
    delay(100);
    M5.Power.powerOff();
  };
  callbacks.onPowerTimeouts =
      [this](unsigned long dimMs, unsigned long screenOffMs,
             unsigned long lightSleepMs, unsigned long powerOffMs) {
        _powerManager.setTimeouts({.dimMs = dimMs,
                                   .screenOffMs = screenOffMs,
                                   .lightSleepMs = lightSleepMs,
                                   .powerOffMs = powerOffMs});
      };
  callbacks.getDeviceStatusJson = [this]() { return deviceStatusJson(); };

  _live.init(callbacks);

  Serial.printf("Capture: %d Hz, %d ms chunks (%u bytes)\n", MIC_SAMPLE_RATE,
                MIC_CHUNK_MS, static_cast<unsigned>(_audio.captureBytes()));
  Serial.printf("Playback: %d Hz, max %d s\n", PLAY_SAMPLE_RATE,
                MAX_PLAYBACK_SEC);

  renderIfNeeded();
  const unsigned long bootElapsedMs = millis() - bootStartMs;
  if (bootElapsedMs < kMinBootDisplayMs) {
    delay(kMinBootDisplayMs - bootElapsedMs);
  }

  connectNetworkStack();
  renderIfNeeded();
}

void AppController::loop() {
  M5.update();

  if (millis() - _lastHeartbeatMs > 3000) {
    _lastHeartbeatMs = millis();
    Serial.printf("[Loop] state=%d region=%d power=%s ws=%d sleep=%d\n",
                  static_cast<int>(_appState), static_cast<int>(_appRegion),
                  powerStateName(_powerManager.getState()), _live.isConnected(),
                  _powerManager.getState() == PowerState::LightSleep);
  }

  if (millis() - _lastHeaderRefreshMs > 30000) {
    _lastHeaderRefreshMs = millis();
    _screenDirty = true;
  }

  updateBatteryStatus();

  if ((_appRegion == AppRegion::Scene || _appState == AppState::Recording ||
       (_faceOnlyMode && _appRegion == AppRegion::Chat)) &&
      millis() - _lastSceneFrameMs > 140) {
    _lastSceneFrameMs = millis();
    _sceneFrame++;
    _screenDirty = true;
  }

  // Reactive face renders at ~30 fps so eye-LERPs (gaze, size, emotion blends)
  // interpolate smoothly between sceneFrame ticks. _sceneFrame stays at 7 Hz
  // so cycle-based events (idle look-around, etc.) keep their wallclock pace.
  const bool reactiveFaceVisible =
      _appState == AppState::Recording ||
      (_faceOnlyMode && _appRegion == AppRegion::Chat &&
       _appState != AppState::Connecting && _appState != AppState::Error &&
       _appState != AppState::ConfirmReset);
  if (reactiveFaceVisible && millis() - _lastFaceRenderMs > 33) {
    _lastFaceRenderMs = millis();
    _screenDirty = true;
  }

  _wifi.poll();
  _live.poll();
  _live.reconnectIfNeeded(_wifi.isConnected() &&
                          _powerManager.getState() != PowerState::LightSleep &&
                          _appState != AppState::Error &&
                          _appRegion != AppRegion::Menu);

  handleButtons();
  processRecording();
  processPlayback();
  processShakeSuggestion();
  processThinkingTimeout();
  processPower();
  processCaptivePortal();
  renderIfNeeded();
  delay(1);
}

void AppController::configureCallbacks() {
  _powerManager.onBrightnessChange(
      [this](int brightness) { _display.setBrightness(brightness); });

  _powerManager.onWiFiStateChange(
      [this](bool enabled) { setNetworkEnabled(enabled); });

  _powerManager.onPowerOff([this]() {
    Serial.println("[Power] Powering off");
    _live.disconnect();
    _wifi.disconnect();
    delay(100);
    M5.Power.powerOff();
  });
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

void AppController::clearToolText() {
  _toolText = "";
  resetBodyPage();
}

void AppController::resetBodyPage() { _bodyPageIndex = 0; }

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

void AppController::handleButtons() {
  const unsigned long now = millis();
  _buttonA.update(M5.BtnA.isPressed(), now);
  _buttonB.update(M5.BtnB.isPressed(), now);

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

  if (_appRegion == AppRegion::Scene) {
    handleSceneButtons();
    return;
  }

  if (_appRegion == AppRegion::Review) {
    handleReviewButtons();
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
    if (_roleplayActive || _voiceMode == "quiz_masters") {
      returnToHomeChat();
    } else {
      openMenu(_appState == AppState::Error ? MenuState::Device : MenuState::Home);
    }
    return;
  }

  if (_buttonB.consumeDoubleClick() && _appState == AppState::Ready) {
    requestClarification();
    return;
  }

  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    const int pageCount = currentBodyPageCount();
    if (pageCount > 1 && _bodyPageIndex < pageCount - 1) {
      Serial.printf("[UI] B tap: advancing text page %d/%d\n",
                    _bodyPageIndex + 2, pageCount);
      _bodyPageIndex++;
      _screenDirty = true;
      return;
    }

    if (_appState != AppState::Recording && _appState != AppState::Error) {
      Serial.println("[UI] B tap on last/no text page: opening flashcard/actions menu");
      openMenu(MenuState::RoleplayActions);
      return;
    }

    if (!_toolText.isEmpty()) {
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
    // A menu click leaves a stale pressed/released event in the button state
    // machine. If the menu closes, Chat mode can otherwise consume that stale
    // press as push-to-talk and send an accidental short/silent clip.
    _buttonA.clearEvents();
    _buttonB.clearEvents();
  }
}

void AppController::handleSceneButtons() {
  if (_appState != AppState::Recording) {
    if (_buttonB.consumeHoldStart()) {
      _powerManager.registerActivity();
      _appRegion = AppRegion::Chat;
      _screenDirty = true;
      return;
    }

    if (_buttonB.consumeClick()) {
      _powerManager.registerActivity();
      openMenu(MenuState::SceneActions);
      return;
    }
  }

  if (_buttonA.consumeClick()) {
    _powerManager.registerActivity();
    promptSceneConversation();
    return;
  }

  if (_buttonA.consumeHoldStart() &&
      (_appState == AppState::Ready || _appState == AppState::Playing ||
       _appState == AppState::Thinking)) {
    startRecording();
    return;
  }

  if (_buttonA.consumeReleased() && _appState == AppState::Recording) {
    stopRecording();
  }
}

void AppController::openMenu(MenuState state) {
  _appRegion = AppRegion::Menu;
  _menuState = state;
  _menuSelection = 0;
  if (state == MenuState::ResumeChat) {
    loadConversationHistory();
  }
  _screenDirty = true;
}

void AppController::handleReviewButtons() {
  if (_buttonB.consumeHoldStart()) {
    _powerManager.registerActivity();
    exitInboxReview();
    return;
  }

  if (_buttonA.consumeClick()) {
    _powerManager.registerActivity();
    if (!_inboxShowingBack) {
      _inboxShowingBack = true;
      _screenDirty = true;
    } else {
      gradeCurrentInboxCard("again");
    }
    return;
  }

  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    if (!_inboxShowingBack) {
      advanceInboxCard();
    } else {
      gradeCurrentInboxCard("good");
    }
  }
}

void AppController::openInboxMenu() {
  openMenu(MenuState::Inbox);
}

void AppController::startInboxReview(const String &mode) {
  _inboxMode = mode == "all" ? "all" : "due";
  _inboxCardCount = 0;
  _inboxIndex = 0;
  _inboxShowingBack = false;
  _inboxDueCount = 0;
  _inboxTotalCount = 0;

  if (!_wifi.isConnected()) {
    _toolText = "Connect WiFi first";
    resetBodyPage();
    closeMenu();
    return;
  }

  if (!_live.fetchInboxFlashcards(_inboxMode, _inboxCards, kMaxInboxFlashcards,
                                  _inboxCardCount, _inboxDueCount,
                                  _inboxTotalCount)) {
    _toolText = "Inbox unavailable";
    resetBodyPage();
    closeMenu();
    return;
  }

  if (_inboxCardCount == 0) {
    _toolText = _inboxMode == "all" ? "Inbox empty" : "No cards due";
    resetBodyPage();
    closeMenu();
    return;
  }

  _appRegion = AppRegion::Review;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  _buttonA.clearEvents();
  _buttonB.clearEvents();
  _screenDirty = true;
}

void AppController::exitInboxReview() {
  _appRegion = AppRegion::Chat;
  _inboxShowingBack = false;
  _inboxIndex = 0;
  _inboxCardCount = 0;
  _buttonA.clearEvents();
  _buttonB.clearEvents();
  _screenDirty = true;
}

void AppController::advanceInboxCard() {
  _inboxShowingBack = false;
  if (_inboxIndex + 1 >= _inboxCardCount) {
    _toolText = "Reviewed " + String(_inboxIndex + 1) + " card" +
                (_inboxIndex == 0 ? "" : "s");
    resetBodyPage();
    exitInboxReview();
    return;
  }
  _inboxIndex++;
  _screenDirty = true;
}

void AppController::gradeCurrentInboxCard(const String &grade) {
  if (_inboxIndex < 0 || _inboxIndex >= _inboxCardCount) {
    return;
  }
  const String cardId = _inboxCards[_inboxIndex].id;
  const bool ok = _live.gradeInboxFlashcard(cardId, grade);
  if (!ok) {
    Serial.printf("[Inbox] grade failed for %s\n", cardId.c_str());
  }
  advanceInboxCard();
}

void AppController::closeMenu() {
  _appRegion = AppRegion::Chat;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  _buttonA.clearEvents();
  _buttonB.clearEvents();
  _screenDirty = true;
}

void AppController::closeMenuToScene() {
  _appRegion = AppRegion::Scene;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  _buttonA.clearEvents();
  _buttonB.clearEvents();
  _lastSceneFrameMs = millis();
  _screenDirty = true;
}

void AppController::saveFlashcardFromScene() {
  const bool sent = _live.sendText(
      "Save the most useful item from our recent exchange as a flashcard. "
      "Pick either your last useful target-language phrase or my last "
      "sentence with its correction. Call the save_flashcard tool if "
      "available; otherwise briefly tell me what you would have saved.");
  closeMenuToScene();
  if (sent) {
    _thinkingStartMs = millis();
    setAppState(AppState::Thinking, "Saving card...");
  } else if (_wifi.isConnected()) {
    setAppState(AppState::Connecting, "Connecting...");
    _live.connect();
  }
}

void AppController::saveFlashcardFromRoleplay() {
  const bool sent = _live.sendText(
      "Save the most useful item from our recent roleplay exchange as a flashcard. "
      "Pick either your last useful German phrase or my last sentence with its "
      "correction. Call the save_flashcard tool if available; otherwise briefly "
      "tell me what you would have saved.");
  closeMenu();
  _roleplayActive = true;
  if (sent) {
    _thinkingStartMs = millis();
    setAppState(AppState::Thinking, "Saving card...");
  } else if (_wifi.isConnected()) {
    setAppState(AppState::Connecting, "Connecting...");
    _live.connect();
  }
}

void AppController::returnToHomeChat() {
  Serial.println("[UI] Returning to clean home chat");
  _powerManager.registerActivity();
  _audio.stopPlayback();
  _appRegion = AppRegion::Chat;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  _roleplayActive = false;
  _readingAssistantIntroPending = false;
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _shakeSuggestionArmed = false;
  _scenePromptPending = false;
  if (_voiceMode != "assistant") {
    _voiceMode = "assistant";
    _settings.setVoiceMode(_voiceMode);
    _live.setVoiceMode(_voiceMode);
  }
  _buttonA.clearEvents();
  _buttonB.clearEvents();
  startFreshConversation();
}

void AppController::navigateBackFromMenu() {
  if (_menuState == MenuState::Home) {
    closeMenu();
    return;
  }

  if (_menuState == MenuState::RoleplayActions) {
    returnToHomeChat();
    return;
  }

  if (_menuState == MenuState::SceneActions) {
    closeMenuToScene();
    return;
  }

  if (_menuState == MenuState::RoleplayStart) {
    openMenu(MenuState::Roleplays);
    return;
  }

  if (_menuState == MenuState::Inbox) {
    openMenu(MenuState::Home);
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
      openMenu(MenuState::Modes);
      return;
    case 2:
      closeMenu();
      startFreshConversation();
      return;
    case 3:
      openMenu(MenuState::ResumeChat);
      return;
    case 4:
      openInboxMenu();
      return;
    case 5:
      loadLearningResourceMenu(MenuState::Lessons);
      return;
    case 6:
      loadLearningResourceMenu(MenuState::Readers);
      return;
    case 7:
      loadLearningResourceMenu(MenuState::Roleplays);
      return;
    case 8:
      openMenu(MenuState::Device);
      return;
    default:
      return;
    }

  case MenuState::Modes:
    switch (_menuSelection) {
    case 0:
      openMenu(MenuState::Home);
      return;
    case 1:
      switchVoiceMode("assistant");
      return;
    case 2:
      switchVoiceMode("quiz_masters");
      return;
    case 3:
      _faceOnlyMode = !_faceOnlyMode;
      _screenDirty = true;
      return;
    case 4:
      openScene(0);
      return;
    case 5:
      openScene(1);
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
    case 3:
      closeMenu();
      showBatteryInfo();
      return;
    case 4:
      _live.disconnect();
      _wifi.disconnect();
      delay(100);
      M5.Power.powerOff();
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

  case MenuState::Lessons:
  case MenuState::Readers:
    if (_menuSelection == 0) {
      openMenu(MenuState::Home);
      return;
    }
    if (_learningResourceCount == 0) {
      return;
    }
    startLearningResource(_menuSelection - 1);
    return;

  case MenuState::Roleplays:
    if (_menuSelection == 0) {
      openMenu(MenuState::Home);
      return;
    }
    if (_learningResourceCount == 0) {
      return;
    }
    _selectedLearningResourceIndex = _menuSelection - 1;
    openMenu(MenuState::RoleplayStart);
    return;

  case MenuState::RoleplayStart:
    if (_menuSelection == 0) {
      openMenu(MenuState::Roleplays);
      return;
    }
    if (_menuSelection == 1 && _selectedLearningResourceIndex >= 0) {
      startLearningResource(_selectedLearningResourceIndex);
    }
    return;

  case MenuState::SceneActions:
    switch (_menuSelection) {
    case 0:
      saveFlashcardFromScene();
      return;
    case 1:
      closeMenuToScene();
      return;
    case 2:
      closeMenu();
      return;
    default:
      return;
    }

  case MenuState::RoleplayActions:
    switch (_menuSelection) {
    case 0:
      saveFlashcardFromRoleplay();
      return;
    case 1:
      closeMenu();
      _roleplayActive = true;
      return;
    case 2:
      Serial.println("[UI] Back to menu: keeping connection open");
      _powerManager.registerActivity();
      _audio.stopPlayback();
      _roleplayActive = false;
      _turnComplete = false;
      _turnHasAudio = false;
      _turnHasModelText = false;
      _turnHasToolActivity = false;
      _shakeSuggestionArmed = false;
      _scenePromptPending = false;
      clearToolText();
      if (_appState == AppState::Thinking || _appState == AppState::Playing) {
        setAppState(AppState::Ready, "Ready");
      }
      openMenu(MenuState::Modes);
      return;
    default:
      return;
    }

  case MenuState::Inbox:
    switch (_menuSelection) {
    case 0:
      openMenu(MenuState::Home);
      return;
    case 1:
      startInboxReview("due");
      return;
    case 2:
      startInboxReview("all");
      return;
    default:
      return;
    }
  }
}

int AppController::menuItemCount() const {
  switch (_menuState) {
  case MenuState::Home:
    return 9;
  case MenuState::Modes:
    return 6;
  case MenuState::Device:
    return 5;
  case MenuState::ResumeChat:
    return _historyCount > 0 ? 1 + _historyCount : 2;
  case MenuState::Lessons:
  case MenuState::Readers:
  case MenuState::Roleplays:
    return _learningResourceCount > 0 ? 1 + _learningResourceCount : 2;
  case MenuState::RoleplayStart:
    return 2;
  case MenuState::SceneActions:
  case MenuState::RoleplayActions:
    return 3;
  case MenuState::Inbox:
    return 3;
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
      return "Modes";
    case 2:
      return "New conversation";
    case 3:
      return "Resume chat";
    case 4:
      return "Inbox";
    case 5:
      return "Lessons";
    case 6:
      return "Readers";
    case 7:
      return "Roleplays";
    case 8:
      return "Device";
    default:
      return "";
    }

  case MenuState::Modes:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return _voiceMode == "assistant" ? "* Assistant" : "Assistant";
    case 2:
      return _voiceMode == "quiz_masters" ? "* Quiz Masters" : "Quiz Masters";
    case 3:
      return _faceOnlyMode ? "* Face-only chat" : "Face-only chat";
    case 4:
      return "Little guy scene";
    case 5:
      return "German flag";
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
      return "Check updates";
    case 3:
      return "Battery info";
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

  case MenuState::Lessons:
  case MenuState::Readers:
  case MenuState::Roleplays:
    if (index == 0) {
      return "Go back";
    }
    if (_learningResourceCount == 0 && index == 1) {
      return _toolText.isEmpty() ? "No resources" : _toolText.substring(0, 26);
    }
    if (index - 1 < _learningResourceCount) {
      const LearningResourceSummary &entry = _learningResources[index - 1];
      String label = entry.title;
      if (!entry.level.isEmpty()) {
        label += " " + entry.level;
      }
      return label.substring(0, 26);
    }
    return "";

  case MenuState::RoleplayStart:
    if (index == 0) {
      return "Go back";
    }
    if (_selectedLearningResourceIndex >= 0 &&
        _selectedLearningResourceIndex < _learningResourceCount) {
      return "Begin conversation";
    }
    return "Begin conversation";

  case MenuState::SceneActions:
  case MenuState::RoleplayActions:
    switch (index) {
    case 0:
      return "Save flashcard";
    case 1:
      return "Back to roleplay";
    case 2:
      return "Back to menu";
    default:
      return "";
    }

  case MenuState::Inbox:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return "Due cards";
    case 2:
      return "All cards";
    default:
      return "";
    }
  }

  return "";
}

void AppController::loadLearningResourceMenu(MenuState targetState) {
  _learningResourceCount = 0;
  if (!_wifi.isConnected()) {
    _toolText = "Connect WiFi first";
    resetBodyPage();
    openMenu(targetState);
    return;
  }

  const bool readers = targetState == MenuState::Readers;
  const bool roleplays = targetState == MenuState::Roleplays;
  const String source = readers ? "graded_reader" : (roleplays ? "roleplay" : "worksheet");
  const String query = readers ? "available graded readers transcripts videos"
                               : (roleplays ? "German roleplays speaking practice cafe doctor train station mock test"
                                            : "German lessons worksheets grammar vocabulary bildbeschreibung");
  const String label = readers ? "Readers" : (roleplays ? "Roleplays" : "Lessons");

  if (!_live.fetchLearningResources(source, query, _learningResources,
                                    kMaxLearningResources,
                                    _learningResourceCount)) {
    _toolText = label + " unavailable";
    resetBodyPage();
  } else if (_learningResourceCount == 0) {
    _toolText = "No " + label;
    resetBodyPage();
  }

  openMenu(targetState);
}

void AppController::startLearningResource(int index) {
  if (index < 0 || index >= _learningResourceCount) {
    return;
  }

  const LearningResourceSummary &entry = _learningResources[index];
  _roleplayActive = entry.source == "roleplay";
  closeMenu();
  _audio.stopPlayback();
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;

  String prompt = "Load learning resource " + entry.resourceId + " and start it. ";
  if (entry.source == "roleplay") {
    prompt += "Start a German-only roleplay immediately. Do not ask me what I want to do. Speak first. In simple German, say who you are and who I am, then begin the scene with your first in-character German line or question. Speak German by default at all times and do not switch to English unless I explicitly ask for English help.";
  } else if (entry.source == "graded_reader") {
    prompt += "Briefly name it, then ask whether to read, search, discuss it, or do checkpoint questions.";
  } else {
    prompt += "Briefly name the lesson, then start with one short guided German practice question.";
  }
  _shakeSuggestionArmed = false;
  if (!_live.sendText(prompt)) {
    if (_wifi.isConnected()) {
      setAppState(AppState::Connecting, "Connecting...");
      _live.connect();
    }
    return;
  }

  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Loading...");
}

void AppController::loadConversationHistory() {
  _historyCount = 0;
  if (!_wifi.isConnected()) {
    _toolText = "Connect WiFi first";
    resetBodyPage();
    return;
  }

  if (!_live.fetchConversationHistory(_history, kMaxConversationHistory,
                                      _historyCount)) {
    _toolText = "History unavailable";
    resetBodyPage();
    return;
  }

  if (_historyCount == 0) {
    _toolText = "No saved chats";
    resetBodyPage();
  }
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
  resetBodyPage();
  closeMenu();
  _live.disconnect();
  setAppState(AppState::Connecting, "Restoring...");
  _live.connect();
}

void AppController::startFreshConversation() {
  _roleplayActive = false;
  _chatId = "";
  _settings.clearChatId();
  _live.setChatId("");
  _quizIntroPending = _voiceMode == "quiz_masters";
  _readingAssistantIntroPending = _voiceMode == "assistant";
  clearToolText();
  _live.disconnect();
  setAppState(AppState::Connecting, "New chat...");
  _live.connect();
}

void AppController::switchVoiceMode(const String &voiceMode) {
  const String normalized =
      voiceMode == "quiz_masters" ? "quiz_masters" : "assistant";

  closeMenu();
  if (_voiceMode == normalized) {
    _toolText = "Mode\n" + voiceModeLabel() + " selected";
    _quizIntroPending = normalized == "quiz_masters";
    resetBodyPage();
    _screenDirty = true;
    maybeSendQuizIntro();
    return;
  }

  _voiceMode = normalized;
  _settings.setVoiceMode(_voiceMode);
  _live.setVoiceMode(_voiceMode);

  // Keep each persona/game from inheriting the previous chat context.
  _chatId = "";
  _settings.clearChatId();
  _live.setChatId("");

  _quizIntroPending = _voiceMode == "quiz_masters";
  _readingAssistantIntroPending = _voiceMode == "assistant";
  _scenePromptPending = false;
  _toolText = "Mode changed\n" + voiceModeLabel();
  resetBodyPage();
  _live.disconnect();
  if (_wifi.isConnected()) {
    setAppState(AppState::Connecting, "Switching mode...");
    _live.connect();
  } else {
    setAppState(AppState::Ready, "Ready");
  }
  _screenDirty = true;
}

void AppController::ensureQuizModeForScene() {
  if (_voiceMode == "quiz_masters") {
    return;
  }

  _voiceMode = "quiz_masters";
  _settings.setVoiceMode(_voiceMode);
  _live.setVoiceMode(_voiceMode);

  // The game scene should not inherit the normal assistant chat context.
  _chatId = "";
  _settings.clearChatId();
  _live.setChatId("");

  _quizIntroPending = false;
  _readingAssistantIntroPending = false;
  _live.disconnect();
  if (_wifi.isConnected()) {
    setAppState(AppState::Connecting, "Starting game...");
    _live.connect();
  }
}

void AppController::promptSceneConversation() {
  if (_appState != AppState::Ready) {
    if (_appState == AppState::Connecting) {
      _scenePromptPending = true;
    }
    return;
  }

  _audio.stopPlayback();
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _shakeSuggestionArmed = false;

  const bool sent = _live.sendText(
      "Start or continue Quiz Masters, a kid-friendly nature quiz game. "
      "Ask one short nature question, wait for my answer, and be cheerful.");
  if (!sent) {
    if (_wifi.isConnected()) {
      setAppState(AppState::Connecting, "Connecting...");
      _live.connect();
    }
    return;
  }

  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Starting quiz...");
}

void AppController::maybeSendReadingAssistantIntro() {
  if (!_readingAssistantIntroPending || _voiceMode != "assistant" ||
      _appState != AppState::Ready) {
    return;
  }

  _readingAssistantIntroPending = false;
  _roleplayActive = true;
  _shakeSuggestionArmed = false;
  _toolText = "Starting\nreading assistant...";
  resetBodyPage();
  _screenDirty = true;

  static constexpr const char *READING_ASSISTANT_HOOKS[] = {
      "Hallo! Ich bin bereit. Was lesen wir heute zusammen? Gibt es ein Buch oder einen Text, über den du sprechen möchtest?",
  };
  static constexpr size_t READING_ASSISTANT_HOOK_COUNT =
      sizeof(READING_ASSISTANT_HOOKS) / sizeof(READING_ASSISTANT_HOOKS[0]);
  const char *hook = READING_ASSISTANT_HOOKS[random(READING_ASSISTANT_HOOK_COUNT)];

  String prompt =
      "For this conversation, become my German reading assistant and roleplay partner. "
      "Start now with this exact hook sentence, then stop and wait for me: \"";
  prompt += hook;
  prompt +=
      "\" After that, help with German reading: vocabulary, grammar, translation, pronunciation, and quick quiz questions when useful. "
      "Keep the opening under 10 seconds.";

  const bool sent = _live.sendText(prompt);
  if (!sent) {
    _roleplayActive = false;
    return;
  }

  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Reading...");
}

void AppController::maybeSendQuizIntro() {
  if (!_quizIntroPending || _voiceMode != "quiz_masters" ||
      _appState != AppState::Ready) {
    return;
  }

  _shakeSuggestionArmed = false;
  const bool sent = _live.sendText(
      "For this conversation, become Quiz Masters: a cheerful, kid-friendly "
      "nature quiz host. Ask one short question at a time, wait for the child's "
      "answer, give kind hints, and keep it fun.");
  if (!sent) {
    return;
  }

  _quizIntroPending = false;
  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Starting quiz...");
}

void AppController::openScene(int sceneKind) {
  closeMenu();
  _audio.stopPlayback();
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  if (_appState == AppState::Playing || _appState == AppState::Thinking) {
    setAppState(AppState::Ready, "Ready");
  }
  _appRegion = AppRegion::Scene;
  _sceneKind = sceneKind;
  if (_sceneKind == 0) {
    ensureQuizModeForScene();
  }
  _sceneFrame = 0;
  _lastSceneFrameMs = millis();
  _screenDirty = true;
}

String AppController::voiceModeLabel() const {
  return _voiceMode == "quiz_masters" ? "Quiz Masters" : "Assistant";
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

void AppController::checkForUpdates() {
  FirmwareUpdateInfo info;
  if (!_wifi.isConnected()) {
    _toolText = "Offline\nCannot check updates";
  } else if (_live.checkFirmwareUpdate(info)) {
    if (info.available) {
      _toolText =
          "Update available\nv" + String(info.latestVersion) + "\n" + info.notes;
    } else {
      _toolText = "Up to date\nv" + String(FIRMWARE_VERSION);
    }
  } else {
    _toolText = "Update check failed";
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::showBatteryInfo() {
  updateBatteryStatus(true);

  _toolText = "Battery\n";
  _toolText += "Display: " + String(_batteryPercent) + "%\n";
  _toolText += "Raw lib: " + String(_batteryRawPercent) + "%\n";
  _toolText += "Bat: " + String(_batteryMv) + " mV\n";
  _toolText += "USB: " + String(_vbusMv) + " mV\n";
  _toolText += "Charge: " + String(chargeStateLabel());

  resetBodyPage();
  _screenDirty = true;
}

void AppController::updateBatteryStatus(bool force) {
  const unsigned long now = millis();
  if (!force && now - _lastBatteryRefreshMs < 5000) {
    return;
  }
  _lastBatteryRefreshMs = now;

  _batteryRawPercent = M5.Power.getBatteryLevel();
  _batteryMv = M5.Power.getBatteryVoltage();
  _vbusMv = M5.Power.getVBUSVoltage();
  _chargeState = static_cast<int>(M5.Power.isCharging());

  const int estimated = estimateBatteryPercentFromVoltage(_batteryMv);
  Serial.printf("[Battery] display=%d raw=%d est=%d bat=%d mV usb=%d mV charge=%s current=%ld mA\n",
                _batteryPercent, _batteryRawPercent, estimated, _batteryMv,
                _vbusMv, chargeStateLabel(),
                static_cast<long>(M5.Power.getBatteryCurrent()));

  const int nextPercent = estimated >= 0 ? estimated : _batteryRawPercent;
  if (nextPercent >= 0 && nextPercent <= 100) {
    if (_batteryPercent < 0 || force) {
      _batteryPercent = nextPercent;
    } else {
      // Smooth display jitter from WiFi/speaker load and charging transitions.
      _batteryPercent = (_batteryPercent * 3 + nextPercent + 2) / 4;
    }
    _screenDirty = true;
  }
}

int AppController::estimateBatteryPercentFromVoltage(int mv) const {
  if (mv <= 0) {
    return -1;
  }

  struct Point {
    int mv;
    int pct;
  };
  static constexpr Point curve[] = {
      {3300, 0},  {3450, 5},  {3550, 12}, {3650, 25},
      {3700, 35}, {3750, 45}, {3800, 55}, {3850, 65},
      {3900, 75}, {4000, 85}, {4100, 95}, {4180, 100},
  };

  if (mv <= curve[0].mv) {
    return curve[0].pct;
  }
  for (int i = 1; i < static_cast<int>(sizeof(curve) / sizeof(curve[0])); i++) {
    if (mv <= curve[i].mv) {
      const Point &lo = curve[i - 1];
      const Point &hi = curve[i];
      return lo.pct + (mv - lo.mv) * (hi.pct - lo.pct) / (hi.mv - lo.mv);
    }
  }
  return 100;
}

const char *AppController::chargeStateLabel() const {
  switch (_chargeState) {
  case static_cast<int>(m5::Power_Class::is_charging):
    return "yes";
  case static_cast<int>(m5::Power_Class::is_discharging):
    return "no";
  default:
    return "unknown";
  }
}

void AppController::startRecording() {
  const AppState previousState = _appState;
  if (previousState == AppState::Thinking || previousState == AppState::Playing) {
    _live.sendCancelTurn(previousState == AppState::Thinking
                             ? "user_interrupted_thinking"
                             : "user_interrupted_playback");
  }

  _shakeSuggestionArmed = false;
  Serial.println("[Rec] === START RECORDING ===");
  _powerManager.registerActivity();
  clearToolText();
  _audio.stopPlayback();
  _audio.startRecording();
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _audioChunksSent = 0;
  _recordingVoiceLevel = 0.0f;
  _recordingTranscript = "";
  _eyeLookX = 0.0f;
  _eyeLookY = 0.0f;
  // Carry the last grammar signal into the next listening face: red becomes a
  // focused/angry face, yellow/gray a sleepy/thinking face, green/default happy.
  if (_turnFeedbackColor == "red") {
    _faceEmotion = 1;
  } else if (_turnFeedbackColor == "yellow" || _turnFeedbackColor == "gray") {
    _faceEmotion = 2;
  } else {
    _faceEmotion = 0;
  }
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

  const int16_t *samples = _audio.captureData();
  const size_t bytes = _audio.captureBytes();
  const size_t sampleCount = bytes / sizeof(int16_t);
  uint64_t absSum = 0;
  int16_t peak = 0;
  for (size_t i = 0; i < sampleCount; i++) {
    const int32_t v = samples[i];
    const int16_t a = static_cast<int16_t>(abs(v));
    absSum += a;
    if (a > peak) {
      peak = a;
    }
  }
  const float avgAbs = sampleCount > 0 ? static_cast<float>(absSum) / sampleCount : 0.0f;
  // Map typical speech energy to 0..1 and smooth it so the face feels alive
  // without flickering wildly between mic chunks.
  const float avgLevel = constrain((avgAbs - 80.0f) / 1400.0f, 0.0f, 1.0f);
  const float peakLevel = constrain((static_cast<float>(peak) - 500.0f) / 9000.0f, 0.0f, 1.0f);
  const float targetLevel = max(avgLevel, peakLevel * 0.65f);
  _recordingVoiceLevel = (_recordingVoiceLevel * 0.65f) + (targetLevel * 0.35f);

  updateReactiveFaceTilt();

  const bool sent = _live.sendAudio(samples, bytes);
  _powerManager.registerActivity();
  _audioChunksSent++;
  _screenDirty = true;
  if (_audioChunksSent <= 5 || _audioChunksSent % 10 == 0) {
    Serial.printf("[Rec] #%d sent=%d\n", _audioChunksSent, sent);
  }
}

void AppController::updateReactiveFaceTilt() {
  if (!M5.Imu.isEnabled()) {
    return;
  }

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) {
    return;
  }

  // Map gravity/tilt into a subtle eye target. Axis signs are chosen for the
  // landscape display orientation; if it feels backwards in hand, swap signs.
  const float targetX = constrain(-ay * 1.2f, -1.0f, 1.0f);
  const float targetY = constrain(ax * 1.2f, -1.0f, 1.0f);
  _eyeLookX = (_eyeLookX * 0.78f) + (targetX * 0.22f);
  _eyeLookY = (_eyeLookY * 0.78f) + (targetY * 0.22f);
}

void AppController::processPlayback() {
  if (_appState != AppState::Thinking && _appState != AppState::Playing) {
    return;
  }

  const int buffered = _audio.bufferedPlaybackBytes();
  const bool hasEnoughToStart = buffered >= kMinPlaybackBytes;
  const bool finalSmallAudioChunk = _turnComplete && buffered > 0;
  if (!_audio.playbackStarted() && (hasEnoughToStart || finalSmallAudioChunk)) {
    _audio.markPlaybackStarted();
    setAppState(AppState::Playing, "Speaking...");
    _audio.advancePlayback();
  }

  if (_audio.playbackStarted()) {
    _audio.advancePlayback();
  }

  if (_appState == AppState::Thinking && _turnComplete && !_turnHasAudio &&
      (_turnHasModelText || _turnHasToolActivity)) {
    // Text-only/tool-only turn. Do not wait for audio that never came.
    _turnComplete = false;
    _turnHasAudio = false;
    _turnHasModelText = false;
    _turnHasToolActivity = false;
    _audio.stopPlayback();
    _shakeSuggestionArmed = true;
    setAppState(AppState::Ready, "Ready");
    return;
  }

  if (_appState == AppState::Playing && _turnComplete &&
      _audio.playbackIdle()) {
    _turnComplete = false;
    _turnHasAudio = false;
    _turnHasModelText = false;
    _turnHasToolActivity = false;
    _audio.stopPlayback();
    _shakeSuggestionArmed = true;
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processShakeSuggestion() {
  if (!_shakeSuggestionArmed || _appState != AppState::Ready ||
      _appRegion == AppRegion::Menu || !_live.isConnected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - _lastShakePollMs < kShakePollMs ||
      now - _lastShakeSuggestionMs < kShakeCooldownMs) {
    return;
  }
  _lastShakePollMs = now;

  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!M5.Imu.isEnabled() || !M5.Imu.getAccel(&ax, &ay, &az)) {
    return;
  }

  if (!_hasAccelSample) {
    _lastAccelX = ax;
    _lastAccelY = ay;
    _lastAccelZ = az;
    _hasAccelSample = true;
    return;
  }

  const float dx = ax - _lastAccelX;
  const float dy = ay - _lastAccelY;
  const float dz = az - _lastAccelZ;
  const float delta = sqrtf(dx * dx + dy * dy + dz * dz);
  _lastAccelX = ax;
  _lastAccelY = ay;
  _lastAccelZ = az;

  if (delta >= kShakeMotionDeltaG) {
    _lastShakeMotionMs = now;
  }

  if (_shakeNeedsStillness) {
    if (now - _lastShakeMotionMs >= kShakeRearmStillMs) {
      _shakeNeedsStillness = false;
      Serial.println("[IMU] Shake rearmed after stillness");
    } else {
      return;
    }
  }

  if (delta < kShakeDeltaG) {
    return;
  }

  Serial.printf("[IMU] Shake detected delta=%.2f g\n", delta);
  _lastShakeSuggestionMs = now;
  _lastShakeMotionMs = now;
  _shakeNeedsStillness = true;
  requestConversationSuggestion();
}

void AppController::requestConversationSuggestion() {
  if (_appState != AppState::Ready || !_live.isConnected()) {
    return;
  }

  _powerManager.registerActivity();
  _audio.stopPlayback();
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _shakeSuggestionArmed = false;
  _toolText = "Getting suggestion...";
  resetBodyPage();
  _screenDirty = true;

  const String prompt =
      "The learner just shook the handheld device because they are stuck after your last turn. "
      "Give exactly one short, useful German sentence or phrase they could say next, "
      "with a brief English meaning. Keep it under 15 seconds. Do not continue the main lesson yet.";

  if (!_live.sendText(prompt)) {
    setAppState(AppState::Ready, "Ready");
    _toolText = "Suggestion unavailable";
    resetBodyPage();
    _screenDirty = true;
    return;
  }

  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Hint...");
}

void AppController::requestClarification() {
  if (_appState != AppState::Ready || !_live.isConnected()) {
    return;
  }

  _powerManager.registerActivity();
  _audio.stopPlayback();
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _shakeSuggestionArmed = false;
  const bool wasCorrection = _toolText.indexOf("!!!!") >= 0;
  _toolText = "Clarifying...";
  resetBodyPage();
  _screenDirty = true;

  String prompt = "The learner pressed the clarification button after your last reply. ";
  if (wasCorrection) {
    prompt += "Your last reply used !!!! as a correction marker. Briefly explain in simple language what was wrong and give the corrected German phrase. Then continue the roleplay with one short German prompt. Keep it under 15 seconds.";
  } else {
    prompt += "Briefly explain what your last German sentence meant or what the learner should do next. Keep it under 15 seconds, then continue with one short German prompt.";
  }

  if (!_live.sendText(prompt)) {
    setAppState(AppState::Ready, "Ready");
    _toolText = "Clarification unavailable";
    resetBodyPage();
    _screenDirty = true;
    return;
  }

  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Clarifying...");
}

void AppController::processThinkingTimeout() {
  if (_appState != AppState::Thinking) {
    return;
  }

  if (millis() - _thinkingStartMs > kThinkingTimeoutMs) {
    Serial.println("[Loop] Thinking timeout");
    _live.sendCancelTurn("firmware_thinking_timeout");
    _turnComplete = false;
    _turnHasAudio = false;
    _turnHasModelText = false;
    _turnHasToolActivity = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
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
  if (!_screenDirty) {
    return;
  }

  _screenDirty = false;
  _display.render(buildDisplayState());
}

DisplayState AppController::buildDisplayState() const {
  DisplayState state;
  state.appState = _appState;
  state.sceneFrame = _sceneFrame;
  if (_appRegion == AppRegion::Scene) {
    state.showScene = true;
    state.sceneFrame = _sceneFrame;
    state.sceneKind = _sceneKind;
    return state;
  }

  if (_appRegion == AppRegion::Review && _inboxCardCount > 0 &&
      _inboxIndex >= 0 && _inboxIndex < _inboxCardCount) {
    state.headerLeft = "Inbox " + String(_inboxIndex + 1) + "/" +
                       String(_inboxCardCount);
    if (_batteryPercent >= 0 && _batteryPercent <= 100) {
      state.headerRight = String(_batteryPercent) + "%";
    }
    const InboxFlashcardSummary &card = _inboxCards[_inboxIndex];
    state.bodyText = _inboxShowingBack
                         ? card.front + "\n---\n" + card.back
                         : card.front;
    state.footerLeft = _inboxShowingBack ? "A again" : "A flip";
    state.footerRight = _inboxShowingBack ? "B good" : "B skip";
    state.pageIndex = _bodyPageIndex;
    state.pageCount = _display.pageCountForText(state.bodyText);
    return state;
  }

  const bool homeMenuVisible =
      _appRegion == AppRegion::Menu && _menuState == MenuState::Home;
  if (homeMenuVisible) {
    state.headerLeft = currentTimeString();
  } else {
    state.headerLeft = _live.activeEndpointLabel();
  }

  if (_batteryPercent >= 0 && _batteryPercent <= 100) {
    state.headerRight = String(_batteryPercent) + "%";
    if (_chargeState == static_cast<int>(m5::Power_Class::is_charging)) {
      state.headerRight += "+";
    }
  } else if (_chargeState == static_cast<int>(m5::Power_Class::is_charging) ||
             _vbusMv > 4000) {
    state.headerRight = "USB";
  }

  state.bodyText = buildBodyText();
  // Normal mode: simple paged assistant text. Face-only mode is a separate UI
  // mode that hides chat text and shows only the reactive face during chat.
  const bool faceOnlyChat = _faceOnlyMode && _appRegion == AppRegion::Chat &&
                            _appState != AppState::Connecting &&
                            _appState != AppState::Error &&
                            _appState != AppState::ConfirmReset;
  state.showReactiveFace = _appState == AppState::Recording || faceOnlyChat;
  state.turnFeedbackColor = _turnFeedbackColor;
  state.footerLeft = _wifi.isConnected() ? _wifi.ssid() : "offline";
  state.footerRight = _chatId.isEmpty() ? "" : _chatId.substring(0, 8);
  state.showRecordingProgress = _appState == AppState::Recording;
  state.recordingProgress = recordingProgress();
  state.voiceLevel = _appState == AppState::Recording ? _recordingVoiceLevel : 0.0f;
  state.eyeLookX = _eyeLookX;
  state.eyeLookY = _eyeLookY;
  state.faceEmotion = _faceEmotion;
  state.faceEyeSpacing = _faceEyeSpacing;
  state.faceAnimSpeed = _faceAnimSpeed;
  state.facePerspective = _facePerspective;
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

  if (_wifi.isCaptivePortalActive()) {
    return "WiFi setup\nJoin " + _wifi.captivePortalSsid() + "\nOpen " +
           _wifi.captivePortalIp();
  }

  switch (_appState) {
  case AppState::Connecting:
    if (_wifi.isConnected()) {
      return _statusText + "\n" + _wifi.ssid() + "\n" + _wifi.localIp();
    }
    return _statusText + "\nSearching WiFi";

  case AppState::Ready:
    if (_roleplayActive || _voiceMode == "quiz_masters") {
      return "Ready\n" + voiceModeLabel() + "\nB page/save\nHold B home";
    }
    return "Ready\n" + voiceModeLabel() + "\nB page/save\nHold B menu";

  case AppState::Recording:
    return "Listening\nRelease A to send";

  case AppState::Thinking:
    return "Thinking\nWaiting for reply";

  case AppState::Playing:
    return "Speaking\nHold A to interrupt";

  case AppState::ConfirmReset:
    return "Factory reset?\nA confirm\nB cancel";

  case AppState::Error:
    if (_errorText.isEmpty()) {
      return String(errorCategoryLabel()) + "\n" + _statusText;
    }
    return String(errorCategoryLabel()) + "\n" + _statusText + "\n" +
           _errorText;
  }

  return "";
}

float AppController::recordingProgress() const {
  if (_appState != AppState::Recording || kMaxRecordingMs == 0) {
    return 0.0f;
  }

  return static_cast<float>(millis() - _recordingStartMs) /
         static_cast<float>(kMaxRecordingMs);
}

int AppController::currentBodyPageCount() const {
  return _display.pageCountForText(buildBodyText());
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
  status["battery_percent"] = _batteryPercent;
  status["battery_raw_percent"] = _batteryRawPercent;
  status["battery_mv"] = _batteryMv;
  status["battery_current_ma"] = M5.Power.getBatteryCurrent();
  status["vbus_mv"] = _vbusMv;
  status["charging"] = chargeStateLabel();
  status["volume"] = _audio.volume();
  status["brightness"] = M5.Display.getBrightness();
  status["voice_mode"] = _voiceMode;
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
