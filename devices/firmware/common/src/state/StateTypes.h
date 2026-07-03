#pragma once

#include <Arduino.h>

/**
 * @brief High-level application states shown by the UI and controller.
 */
enum class AppState {
  /// Establishing device services and network connectivity.
  Connecting,

  /// Idle and ready for push-to-talk input.
  Ready,

  /// Recording microphone audio for the current turn.
  Recording,

  /// Waiting for the remote model to respond.
  Thinking,

  /// Playing back assistant audio.
  Playing,

  /// Waiting for user confirmation before factory reset.
  ConfirmReset,

  /// Showing an error state that needs attention or retry.
  Error,

  /// Showing a local timer alarm that can be dismissed with either button.
  Alarm,
};

/// Maximum number of menu rows rendered at once.
constexpr int MAX_MENU_VISIBLE_ITEMS = 4;

/**
 * @brief Complete UI snapshot consumed by TextDisplay::render().
 */
struct DisplayState {
  /// Current top-level application state.
  AppState appState = AppState::Connecting;

  /// Left-aligned header text.
  String headerLeft;

  /// Right-aligned header text.
  String headerRight;

  /// Main body text content.
  String bodyText;

  /// Left-aligned footer text.
  String footerLeft;

  /// Right-aligned footer text.
  String footerRight;

  /// Color name for the small instant-feedback indicator.
  String turnFeedbackColor;

  /// Whether to show the recording duration indicator.
  bool showRecordingProgress = false;

  /// Recording progress from 0.0 to 1.0.
  float recordingProgress = 0.0f;

  /// Recent microphone energy from 0.0 to 1.0 for reactive face motion.
  float voiceLevel = 0.0f;

  /// Reactive face horizontal gaze target from -1.0 to 1.0.
  float eyeLookX = 0.0f;

  /// Reactive face vertical gaze target from -1.0 to 1.0.
  float eyeLookY = 0.0f;

  /// Reactive face emotion: 0=default, 1=focused, 2=sleepy/thinking.
  int faceEmotion = 0;

  /// Horizontal spacing between the two reactive eyes.
  float faceEyeSpacing = 65.0f;

  /// Animation speed multiplier for the reactive face.
  float faceAnimSpeed = 1.0f;

  /// Pseudo-3D perspective strength for the reactive face.
  float facePerspective = 3.0f;

  /// Whether TextDisplay should render the reactive eyes instead of text.
  bool showReactiveFace = false;

  /// Whether body text should be rendered dimmed.
  bool bodyDim = false;

  /// Whether the menu overlay is currently visible.
  bool showMenu = false;

  /// Whether TextDisplay should render one of the pixel-art scenes.
  bool showScene = false;

  /// Animation frame for scene and reactive-face rendering.
  int sceneFrame = 0;

  /// Scene kind: 0=little guy scene, 1=German flag pixel art.
  int sceneKind = 0;

  /// Visible menu item labels.
  String menuItems[MAX_MENU_VISIBLE_ITEMS];

  /// Number of valid menu entries in menuItems.
  int menuItemCount = 0;

  /// Index of the selected menu item within the visible page.
  int menuSelectedIndex = 0;

  /// Whether there are menu items above the current visible page.
  bool menuHasMoreAbove = false;

  /// Whether there are menu items below the current visible page.
  bool menuHasMoreBelow = false;

  /// Current body page index.
  int pageIndex = 0;

  /// Total number of available body pages.
  int pageCount = 1;

  /// Whether page 0 is occupied by a stored image before bodyText pages.
  bool imagePresent = false;

  /// Whether TextDisplay should render the timer alarm screen.
  bool alarmActive = false;

  /// Alarm title shown on the dedicated alarm screen.
  String alarmTitle;

  /// Optional alarm detail line, usually a comma-separated timer name list.
  String alarmDetail;
};
