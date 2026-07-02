#pragma once

#include <Arduino.h>

enum class AppState {
  Connecting,
  Ready,
  Recording,
  Thinking,
  Playing,
  ConfirmReset,
  Error,
};

constexpr int MAX_MENU_VISIBLE_ITEMS = 4;

struct DisplayState {
  AppState appState = AppState::Connecting;
  String headerLeft;
  String headerRight;
  String bodyText;
  String footerLeft;
  String footerRight;
  String turnFeedbackColor;
  bool showRecordingProgress = false;
  float recordingProgress = 0.0f;
  float voiceLevel = 0.0f;
  float eyeLookX = 0.0f;
  float eyeLookY = 0.0f;
  int faceEmotion = 0; // 0=default, 1=angry/focused, 2=eepy/sleepy
  float faceEyeSpacing = 65.0f;
  float faceAnimSpeed = 1.0f;
  float facePerspective = 1.0f;
  bool showReactiveFace = false;
  bool showMenu = false;
  bool showScene = false;
  int sceneFrame = 0;
  int sceneKind = 0; // 0 = little guy scene, 1 = German flag pixel art
  String menuItems[MAX_MENU_VISIBLE_ITEMS];
  int menuItemCount = 0;
  int menuSelectedIndex = 0;
  bool menuHasMoreAbove = false;
  bool menuHasMoreBelow = false;
  int pageIndex = 0;
  int pageCount = 1;
};
