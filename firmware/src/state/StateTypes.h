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

  /// Whether body text should be rendered dimmed.
  bool bodyDim = false;

  /// Whether the menu overlay is currently visible.
  bool showMenu = false;

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
};
