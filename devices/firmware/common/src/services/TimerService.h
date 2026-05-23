#pragma once

#include "Config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

/**
 * @brief Persisted countdown timer record.
 */
struct TimerRecord {
  /// Stable monotonic id, never reused.
  uint32_t id = 0;

  /// Optional timer name; empty for unnamed timers.
  String name;

  /// Wall-clock deadline in seconds since the Unix epoch.
  time_t deadlineEpoch = 0;

  /// Original configured duration in seconds.
  uint32_t durationSeconds = 0;

  /// Wall-clock creation time; preserved across extends.
  time_t createdAtEpoch = 0;
};

/**
 * @brief Reference to a specific timer for cancel/extend operations.
 *
 * Exactly one of id or name should be set. If both are empty and only one timer
 * is active, the active timer is selected.
 */
struct TimerRef {
  /// Timer id, or 0 when unspecified.
  uint32_t id = 0;

  /// Timer name, or empty when unspecified.
  String name;
};

/**
 * @brief Persistent local countdown timer store.
 */
class TimerService {
public:
  /**
   * @brief Result codes for timer mutations and lookup operations.
   */
  enum class Result {
    /// Operation completed successfully.
    Ok,

    /// Wall clock is not valid enough to schedule timers.
    ClockNotSynced,

    /// Requested duration is outside configured bounds.
    InvalidDuration,

    /// The active timer limit has been reached.
    LimitReached,

    /// A named timer with the same normalized name already exists.
    DuplicateName,

    /// The requested timer could not be found.
    NotFound,

    /// The request did not uniquely identify one timer.
    Ambiguous,

    /// No timers are currently active.
    NoTimers,
  };

  /// Initialize persistent storage and load active timers.
  void init();

  /// Number of active timers.
  int count() const { return _count; }

  /**
   * @brief Read an active timer by index.
   * @param index Zero-based active timer index.
   * @return Timer record at @p index.
   */
  const TimerRecord &at(int index) const { return _timers[index]; }

  /**
   * @brief Add a new countdown timer.
   * @param durationSeconds Requested duration in seconds.
   * @param name Optional display/tool name.
   * @param outCreated Receives the created timer on success.
   * @return Operation result.
   */
  Result addTimer(uint32_t durationSeconds, const String &name,
                  TimerRecord &outCreated);

  /**
   * @brief Cancel one active timer.
   * @param ref Timer selector. If empty and one timer is active, that timer wins.
   * @param outCancelled Receives the removed timer on success.
   * @return Operation result.
   */
  Result cancel(const TimerRef &ref, TimerRecord &outCancelled);

  /**
   * @brief Cancel all active timers.
   * @return Number of timers cancelled.
   */
  int cancelAll();

  /**
   * @brief Adjust one timer deadline by a signed number of seconds.
   * @param ref Timer selector using the same disambiguation rules as cancel().
   * @param deltaSeconds Seconds to add to or remove from the current deadline.
   * @param outAdjusted Receives the adjusted timer on success.
   * @return Operation result.
   */
  Result extend(const TimerRef &ref, int32_t deltaSeconds,
                TimerRecord &outAdjusted);

  /**
   * @brief Collect expired timers, remove them from storage, and persist.
   * @param now Current wall-clock time.
   * @param out Output buffer for expired timer records.
   * @param maxOut Capacity of @p out.
   * @return Number of expired timers written into @p out.
   */
  int harvestExpired(time_t now, TimerRecord *out, int maxOut);

  /**
   * @brief Find the earliest active timer deadline.
   * @return Earliest deadline epoch, or 0 when no timers are active.
   */
  time_t nextDeadline() const;

  /**
   * @brief Build a JSON payload describing every active timer.
   * @param now Current wall-clock time used to calculate remaining seconds.
   * @return Serialized timer summary for tool responses.
   */
  String describeAll(time_t now) const;

  /**
   * @brief Human-readable result description for tool responses.
   * @param r Result code to describe.
   * @return Static result description string.
   */
  static const char *describeResult(Result r);

private:
  /// Preferences namespace handle for timer persistence.
  Preferences _prefs;

  /// Whether Preferences storage opened successfully.
  bool _prefsReady = false;

  /// Active timer records.
  TimerRecord _timers[MAX_TIMERS];

  /// Number of active entries in _timers.
  int _count = 0;

  /// Next monotonic timer id to allocate.
  uint32_t _nextId = 1;

  /**
   * @brief Find a timer by normalized name.
   * @param name Timer name to match.
   * @return Timer index, or -1 when not found.
   */
  int findByName(const String &name) const;

  /**
   * @brief Find a timer by id.
   * @param id Timer id to match.
   * @return Timer index, or -1 when not found.
   */
  int findById(uint32_t id) const;

  /**
   * @brief Resolve a timer reference to an active timer index.
   * @param ref Timer selector.
   * @param outIndex Receives the timer index on success.
   * @return Ok, NoTimers, NotFound, or Ambiguous.
   */
  Result resolveRef(const TimerRef &ref, int &outIndex) const;

  /// Persist the active timer list to Preferences storage.
  void persist();

  /// Load active timers from Preferences storage.
  void load();

  /**
   * @brief Remove one timer by index without persisting.
   * @param index Active timer index to remove.
   */
  void removeAt(int index);
};
