#pragma once

#include "../Config.h"
#include "../input/ButtonStateMachine.h"
#include "../power/PowerManager.h"
#include "../services/AudioService.h"
#include "../services/LiveSessionService.h"
#include "../services/SettingsStore.h"
#include "../services/TimerService.h"
#include "../services/WiFiService.h"
#include "../state/StateTypes.h"
#include "../ui/TextDisplay.h"
#include <M5PM1.h>
#include <esp_sleep.h>
#include <time.h>

class AppController {
public:
  struct AlarmStep {
    int freqHz;
    int durationMs;
  };

  void setup();
  void loop();

private:
  enum class AppRegion { Initializing, Chat, Menu };

  enum class ErrorCategory {
    None,
    Startup,
    WiFiTimeout,
    ServerRefused,
    GeminiUnavailable,
  };

  enum class MenuState { Home, Device, ResumeChat };

  static constexpr int kMinPlaybackBytes = PLAY_SAMPLE_RATE * 2 / 4;
  static constexpr unsigned long kThinkingTimeoutMs = 15000;
  static constexpr unsigned long kMaxRecordingMs = 30000;
  static constexpr unsigned long kResetHoldMs = 1500;
  static constexpr int kMaxConversationHistory = 10;
  static constexpr int kMaxExpiredPerWake = MAX_TIMERS;
  static constexpr unsigned long kTextRevealFrameMs = 18;

  AppRegion _appRegion = AppRegion::Initializing;
  AppState _appState = AppState::Connecting;
  MenuState _menuState = MenuState::Home;
  String _statusText = "Starting...";
  String _errorText;
  String _chatId;
  String _toolText;
  String _toolTextRevealTarget;
  String _toolTextRevealLayout;
  bool _turnComplete = false;
  bool _turnHasAudio = false;
  bool _pendingTurnReset = false;
  bool _imagePresent = false;
  bool _screenDirty = true;
  bool _startupChecklistVisible = true;
  bool _startupPowerDone = false;
  bool _startupWifiDone = false;
  bool _startupInternetDone = false;
  unsigned long _thinkingStartMs = 0;
  unsigned long _recordingStartMs = 0;
  unsigned long _lastTextRevealMs = 0;
  unsigned long _resetHoldStartMs = 0;
  unsigned long _lastHeartbeatMs = 0;
  unsigned long _lastHeaderRefreshMs = 0;
  int _audioChunksSent = 0;
  int _bodyPageIndex = 0;
  int _toolTextRevealIndex = 0;
  int _menuSelection = 0;
  int _historyCount = 0;
  ErrorCategory _errorCategory = ErrorCategory::None;
  AppState _resetReturnState = AppState::Ready;
  String _resetReturnStatus;
  String _resetReturnError;
  ErrorCategory _resetReturnCategory = ErrorCategory::None;
  ConversationSummary _history[kMaxConversationHistory];

  ButtonStateMachine _buttonA = ButtonStateMachine(500, 350);
  ButtonStateMachine _buttonB = ButtonStateMachine(1000, 350);

  TextDisplay _display;
  PowerManager _powerManager;
  WiFiService _wifi;
  AudioService _audio;
  LiveSessionService _live;
  SettingsStore _settings;
  TimerService _timers;
  M5PM1 _pm1;
  bool _pm1Ready = false;
  unsigned long _lastPm1PollMs = 0;

  // Alarm runtime state — populated when AppState::Alarm is active.
  AppState _alarmReturnState = AppState::Ready;
  String _alarmReturnStatus;
  String _alarmTitle;
  String _alarmDetail;
  int _alarmStepIndex = 0;
  unsigned long _alarmStepStartMs = 0;
  bool _alarmStepInFlight = false;
  bool _networkStackStarted = false;
  bool _bootHadExpiredAlarm = false;

  void configureCallbacks();
  void connectNetworkStack();
  void setNetworkEnabled(bool enabled);
  void setAppState(AppState state, const String &status = "",
                   const String &error = "");
  void setErrorState(ErrorCategory category, const String &status,
                     const String &error);
  void retryAfterError();
  void performPowerOff(bool allowIdleDeepSleep = false);
  void shutdownHardware();
  bool shouldPowerOffAfterIdleDeepSleep(
      esp_sleep_wakeup_cause_t wakeCause) const;
  void clearToolText();
  void setToolTextImmediate(const String &text);
  void startToolTextReveal(const String &text);
  void appendToolTextReveal(const String &text);
  void rebuildToolTextRevealLayout();
  void completeToolTextReveal();
  void cancelToolTextReveal();
  void resetBodyPage();
  void beginFactoryReset();
  const char *errorCategoryLabel() const;

  void handleButtons();
  void handleChatButtons();
  void handleMenuButtons();
  void startRecording();
  void stopRecording();
  void processRecording();
  void processPlayback();
  void processThinkingTimeout();
  void processTextReveal();
  void processPower();
  void processCaptivePortal();
  void renderIfNeeded();
  int currentBodyPageCount() const;

  void openMenu(MenuState state = MenuState::Home);
  void closeMenu();
  void navigateBackFromMenu();
  void cycleMenuSelection();
  void selectCurrentMenuItem();
  int menuItemCount() const;
  String menuItemLabel(int index) const;
  void loadConversationHistory();
  void resumeConversation(int index);
  void startFreshConversation();
  void startCaptivePortalFlow();
  void checkForUpdates();

  DisplayState buildDisplayState() const;
  String buildBodyText() const;
  String buildStartupChecklistText() const;
  String deviceStatusJson() const;
  String currentTimeString() const;

  // Timer / alarm
  void onTimersChanged();
  void checkTimerExpiry();
  void enterAlarmState(const String &title, const String &detail);
  void exitAlarmState();
  void serviceAlarmTrill();
  bool enterDeepSleepForTimerOrIdle(bool includeIdleShutdownDeadline);
  bool handleAlarmButtons();
  String handleSetTimerTool(int durationSeconds, const String &name);
  String handleListTimersTool();
  String handleCancelTimerTool(const TimerRef &ref, bool all);
  String handleExtendTimerTool(int deltaSeconds, const TimerRef &ref);
  String formatTimerSummary(time_t now) const;

  static const AlarmStep *alarmPattern();
  static int alarmPatternLen();
};
