#pragma once

#include "../Config.h"
#include "../input/ButtonStateMachine.h"
#include "../power/PowerManager.h"
#include "../services/AudioService.h"
#include "../services/LiveSessionService.h"
#include "../services/SettingsStore.h"
#include "../services/WiFiService.h"
#include "../state/StateTypes.h"
#include "../ui/TextDisplay.h"

class AppController {
public:
  void setup();
  void loop();

private:
  enum class AppRegion { Initializing, Chat, Menu, Scene, Review };

  enum class ErrorCategory {
    None,
    Startup,
    WiFiTimeout,
    ServerRefused,
    GeminiUnavailable,
  };

  enum class MenuState {
    Home,
    Modes,
    Device,
    ResumeChat,
    Lessons,
    Readers,
    Roleplays,
    RoleplayStart,
    SceneActions,
    RoleplayActions,
    Inbox
  };

  static constexpr int kMinPlaybackBytes = PLAY_SAMPLE_RATE * 2 / 4;
  static constexpr unsigned long kThinkingTimeoutMs = 15000;
  static constexpr unsigned long kMaxRecordingMs = 30000;
  static constexpr unsigned long kResetHoldMs = 1500;
  static constexpr unsigned long kMinBootDisplayMs = 800;
  static constexpr unsigned long kShakePollMs = 80;
  static constexpr unsigned long kShakeCooldownMs = 6000;
  static constexpr unsigned long kShakeRearmStillMs = 2000;
  // Shake-to-suggest should require a deliberate shake, not normal hand motion.
  static constexpr float kShakeDeltaG = 3.0f;
  static constexpr float kShakeMotionDeltaG = 0.8f;
  static constexpr int kMaxConversationHistory = 10;
  static constexpr int kMaxLearningResources = 8;
  static constexpr int kMaxInboxFlashcards = 20;

  AppRegion _appRegion = AppRegion::Initializing;
  AppState _appState = AppState::Connecting;
  MenuState _menuState = MenuState::Home;
  String _statusText = "Starting...";
  String _errorText;
  String _chatId;
  String _voiceMode = "assistant";
  String _toolText;
  String _turnFeedbackColor;
  String _turnFeedbackCorrection;
  String _turnFeedbackReason;
  String _recordingTranscript;
  bool _turnComplete = false;
  bool _turnHasAudio = false;
  bool _turnHasModelText = false;
  bool _turnHasToolActivity = false;
  bool _quizIntroPending = false;
  bool _readingAssistantIntroPending = true;
  bool _scenePromptPending = false;
  bool _roleplayActive = false;
  bool _faceOnlyMode = false;
  bool _shakeSuggestionArmed = false;
  bool _shakeNeedsStillness = false;
  bool _hasAccelSample = false;
  bool _screenDirty = true;
  unsigned long _thinkingStartMs = 0;
  unsigned long _recordingStartMs = 0;
  unsigned long _resetHoldStartMs = 0;
  unsigned long _lastHeartbeatMs = 0;
  unsigned long _lastHeaderRefreshMs = 0;
  unsigned long _lastSceneFrameMs = 0;
  unsigned long _lastFaceRenderMs = 0;
  unsigned long _lastBatteryRefreshMs = 0;
  unsigned long _lastShakePollMs = 0;
  unsigned long _lastShakeSuggestionMs = 0;
  unsigned long _lastShakeMotionMs = 0;
  int _audioChunksSent = 0;
  int _sceneFrame = 0;
  int _sceneKind = 0;
  int _batteryPercent = -1;
  int _batteryRawPercent = -1;
  int _batteryMv = 0;
  int _vbusMv = 0;
  int _chargeState = 2;
  float _recordingVoiceLevel = 0.0f;
  float _eyeLookX = 0.0f;
  float _eyeLookY = 0.0f;
  int _faceEmotion = 0;
  float _faceEyeSpacing = 65.0f;
  float _faceAnimSpeed = 1.0f;
  float _facePerspective = 3.0f;
  float _lastAccelX = 0.0f;
  float _lastAccelY = 0.0f;
  float _lastAccelZ = 0.0f;
  int _bodyPageIndex = 0;
  int _menuSelection = 0;
  int _historyCount = 0;
  int _learningResourceCount = 0;
  int _selectedLearningResourceIndex = -1;
  int _inboxCardCount = 0;
  int _inboxIndex = 0;
  int _inboxDueCount = 0;
  int _inboxTotalCount = 0;
  bool _inboxShowingBack = false;
  String _inboxMode = "due";
  ErrorCategory _errorCategory = ErrorCategory::None;
  AppState _resetReturnState = AppState::Ready;
  String _resetReturnStatus;
  String _resetReturnError;
  ErrorCategory _resetReturnCategory = ErrorCategory::None;
  ConversationSummary _history[kMaxConversationHistory];
  LearningResourceSummary _learningResources[kMaxLearningResources];
  InboxFlashcardSummary _inboxCards[kMaxInboxFlashcards];

  ButtonStateMachine _buttonA = ButtonStateMachine(500, 350);
  ButtonStateMachine _buttonB = ButtonStateMachine(1000, 350);

  TextDisplay _display;
  PowerManager _powerManager;
  WiFiService _wifi;
  AudioService _audio;
  LiveSessionService _live;
  SettingsStore _settings;

  void configureCallbacks();
  void connectNetworkStack();
  void setNetworkEnabled(bool enabled);
  void setAppState(AppState state, const String &status = "",
                   const String &error = "");
  void setErrorState(ErrorCategory category, const String &status,
                     const String &error);
  void retryAfterError();
  void clearToolText();
  void resetBodyPage();
  void restoreSessionPreview();
  void beginFactoryReset();
  const char *errorCategoryLabel() const;

  void handleButtons();
  void handleChatButtons();
  void handleMenuButtons();
  void handleSceneButtons();
  void handleReviewButtons();
  void startRecording();
  void stopRecording();
  void processRecording();
  void updateReactiveFaceTilt();
  void processPlayback();
  void processShakeSuggestion();
  void requestConversationSuggestion();
  void requestClarification();
  void processThinkingTimeout();
  void processPower();
  void processCaptivePortal();
  void renderIfNeeded();
  float recordingProgress() const;
  int currentBodyPageCount() const;

  void openMenu(MenuState state = MenuState::Home);
  void closeMenu();
  void closeMenuToScene();
  void saveFlashcardFromScene();
  void saveFlashcardFromRoleplay();
  void returnToHomeChat();
  void navigateBackFromMenu();
  void cycleMenuSelection();
  void selectCurrentMenuItem();
  int menuItemCount() const;
  String menuItemLabel(int index) const;
  void loadConversationHistory();
  void loadLearningResourceMenu(MenuState targetState);
  void startLearningResource(int index);
  void openInboxMenu();
  void startInboxReview(const String &mode);
  void exitInboxReview();
  void advanceInboxCard();
  void gradeCurrentInboxCard(const String &grade);
  void resumeConversation(int index);
  void startFreshConversation();
  void switchVoiceMode(const String &voiceMode);
  void ensureQuizModeForScene();
  void promptSceneConversation();
  void maybeSendQuizIntro();
  void maybeSendReadingAssistantIntro();
  void openScene(int sceneKind = 0);
  String voiceModeLabel() const;
  void startCaptivePortalFlow();
  void checkForUpdates();
  void showBatteryInfo();
  void updateBatteryStatus(bool force = false);
  int estimateBatteryPercentFromVoltage(int mv) const;
  const char *chargeStateLabel() const;

  DisplayState buildDisplayState() const;
  String buildBodyText() const;
  String deviceStatusJson() const;
  String currentTimeString() const;
};
