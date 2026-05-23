#pragma once

#include "../Config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <time.h>

struct TimerRecord {
  uint32_t id = 0;            // stable monotonic id, never reused
  String name;                // empty for unnamed
  time_t deadlineEpoch = 0;   // wall-clock seconds since unix epoch
  uint32_t durationSeconds = 0;
  time_t createdAtEpoch = 0;  // when the timer was first started; preserved across extends
};

// Reference to a specific timer for cancel/extend. Exactly one of id or name
// should be set; if both are empty and only one timer is active, that one wins.
struct TimerRef {
  uint32_t id = 0;             // 0 = not specified
  String name;                 // empty = not specified
};

class TimerService {
public:
  enum class Result {
    Ok,
    ClockNotSynced,
    InvalidDuration,
    LimitReached,
    DuplicateName,
    NotFound,
    Ambiguous,
    NoTimers,
  };

  void init();

  int count() const { return _count; }
  const TimerRecord &at(int index) const { return _timers[index]; }

  Result addTimer(uint32_t durationSeconds, const String &name,
                  TimerRecord &outCreated);

  // Cancel a single timer. If ref is empty and exactly one timer is active,
  // that one is cancelled.
  Result cancel(const TimerRef &ref, TimerRecord &outCancelled);
  int cancelAll();

  // Adjust deadline by delta seconds. Same disambiguation rules as cancel.
  Result extend(const TimerRef &ref, int32_t deltaSeconds,
                TimerRecord &outAdjusted);

  // Collects timers whose deadline is <= now, removes them from the active
  // list, and persists. Returns the number written into out[].
  int harvestExpired(time_t now, TimerRecord *out, int maxOut);

  // Earliest active deadline, or 0 when no timers are running.
  time_t nextDeadline() const;

  // JSON-ish payload describing every active timer; used as the tool response.
  String describeAll(time_t now) const;

  static const char *describeResult(Result r);

private:
  Preferences _prefs;
  bool _prefsReady = false;
  TimerRecord _timers[MAX_TIMERS];
  int _count = 0;
  uint32_t _nextId = 1;

  int findByName(const String &name) const;
  int findById(uint32_t id) const;
  // Resolve a TimerRef to an index. Returns Ok/NoTimers/NotFound/Ambiguous.
  Result resolveRef(const TimerRef &ref, int &outIndex) const;
  void persist();
  void load();
  void removeAt(int index);
};
