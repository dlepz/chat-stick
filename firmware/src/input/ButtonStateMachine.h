#pragma once

#include <Arduino.h>

/**
 * @brief Tracks button state and derives click, double-click, and hold events.
 */
class ButtonStateMachine {
public:
  /**
   * @brief Timing configuration for derived button events.
   */
  struct Config {
    /// Milliseconds a press must last before it counts as a hold.
    unsigned long holdMs;

    /// Maximum gap between clicks to emit a double-click event.
    unsigned long doubleClickMs;
  };

  /// Construct with default hold and double-click timings.
  ButtonStateMachine() : _config({500, 350}) {}

  /**
   * @brief Construct with explicit hold and double-click timings.
   * @param holdMs Milliseconds required to enter the held state.
   * @param doubleClickMs Maximum gap between clicks for a double click.
   */
  ButtonStateMachine(unsigned long holdMs, unsigned long doubleClickMs = 350)
      : _config({holdMs, doubleClickMs}) {}

  /**
   * @brief Construct from a full configuration struct.
   * @param config Button timing configuration.
   */
  explicit ButtonStateMachine(const Config &config) : _config(config) {}

  /**
   * @brief Replace the timing configuration.
   * @param config New button timing configuration.
   */
  void setConfig(const Config &config) { _config = config; }

  /**
   * @brief Advance the state machine with the current physical button state.
   * @param pressed Whether the button is physically pressed.
   * @param nowMs Current timestamp in milliseconds.
   */
  void update(bool pressed, unsigned long nowMs);

  /// Clear all latched edge events without changing held/pressed state.
  void clearEvents();

  /// Whether the button is currently pressed.
  bool isPressed() const { return _pressed; }

  /// Whether the button is currently in the held state.
  bool isHeld() const { return _held; }

  /// Consume and clear the pressed event.
  bool consumePressed();

  /// Consume and clear the released event.
  bool consumeReleased();

  /// Consume and clear the single-click event.
  bool consumeClick();

  /// Consume and clear the double-click event.
  bool consumeDoubleClick();

  /// Consume and clear the hold-start event.
  bool consumeHoldStart();

  /// Consume and clear the hold-release event.
  bool consumeHoldRelease();

private:
  /// Configured event timing thresholds.
  Config _config;

  /// Whether the physical button is currently down.
  bool _pressed = false;

  /// Whether the current press has crossed the hold threshold.
  bool _held = false;

  /// Whether a second click may still arrive for a double click.
  bool _awaitingSecondClick = false;

  /// Timestamp when the current press started.
  unsigned long _pressStartMs = 0;

  /// Timestamp of the most recent release.
  unsigned long _lastReleaseMs = 0;

  /// Latched press edge event.
  bool _pressedEvent = false;

  /// Latched release edge event.
  bool _releasedEvent = false;

  /// Latched single-click event.
  bool _clickEvent = false;

  /// Latched double-click event.
  bool _doubleClickEvent = false;

  /// Latched hold-start event.
  bool _holdStartEvent = false;

  /// Latched hold-release event.
  bool _holdReleaseEvent = false;
};
