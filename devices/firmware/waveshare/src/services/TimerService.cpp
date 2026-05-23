#include "TimerService.h"

#include "../Config.h"
#include <ArduinoJson.h>

namespace {
constexpr const char *kNamespace = "timers";
constexpr const char *kBlobKey = "list";

bool equalsIgnoreCaseTrim(const String &a, const String &b) {
  String aa = a;
  String bb = b;
  aa.trim();
  bb.trim();
  aa.toLowerCase();
  bb.toLowerCase();
  return aa == bb;
}
} // namespace

void TimerService::init() {
  _count = 0;
  _prefsReady = _prefs.begin(kNamespace, false);
  if (!_prefsReady) {
    Serial.println("[Timers] Preferences init failed");
    return;
  }
  load();
}

TimerService::Result TimerService::addTimer(uint32_t durationSeconds,
                                            const String &name,
                                            TimerRecord &outCreated) {
  if (durationSeconds < TIMER_MIN_DURATION_SEC ||
      durationSeconds > TIMER_MAX_DURATION_SEC) {
    return Result::InvalidDuration;
  }

  const time_t now = time(nullptr);
  if (now < TIMER_MIN_VALID_EPOCH) {
    return Result::ClockNotSynced;
  }

  if (_count >= MAX_TIMERS) {
    return Result::LimitReached;
  }

  String trimmed = name;
  trimmed.trim();
  if (trimmed.length() > TIMER_NAME_MAX_LEN) {
    trimmed = trimmed.substring(0, TIMER_NAME_MAX_LEN);
  }
  if (!trimmed.isEmpty() && findByName(trimmed) >= 0) {
    return Result::DuplicateName;
  }

  TimerRecord &slot = _timers[_count];
  slot.id = _nextId++;
  slot.name = trimmed;
  slot.durationSeconds = durationSeconds;
  slot.deadlineEpoch = now + static_cast<time_t>(durationSeconds);
  slot.createdAtEpoch = now;
  _count++;
  persist();

  outCreated = slot;
  return Result::Ok;
}

TimerService::Result TimerService::resolveRef(const TimerRef &ref,
                                              int &outIndex) const {
  if (_count == 0) return Result::NoTimers;

  if (ref.id > 0) {
    const int idx = findById(ref.id);
    if (idx < 0) return Result::NotFound;
    outIndex = idx;
    return Result::Ok;
  }

  String trimmedName = ref.name;
  trimmedName.trim();
  if (!trimmedName.isEmpty()) {
    const int idx = findByName(trimmedName);
    if (idx < 0) return Result::NotFound;
    outIndex = idx;
    return Result::Ok;
  }

  if (_count > 1) return Result::Ambiguous;
  outIndex = 0;
  return Result::Ok;
}

TimerService::Result TimerService::cancel(const TimerRef &ref,
                                          TimerRecord &outCancelled) {
  int index = -1;
  const Result rc = resolveRef(ref, index);
  if (rc != Result::Ok) return rc;

  outCancelled = _timers[index];
  removeAt(index);
  persist();
  return Result::Ok;
}

int TimerService::cancelAll() {
  const int n = _count;
  _count = 0;
  for (int i = 0; i < MAX_TIMERS; i++) {
    _timers[i] = TimerRecord{};
  }
  persist();
  return n;
}

TimerService::Result TimerService::extend(const TimerRef &ref,
                                          int32_t deltaSeconds,
                                          TimerRecord &outAdjusted) {
  int index = -1;
  const Result rc = resolveRef(ref, index);
  if (rc != Result::Ok) return rc;

  TimerRecord &t = _timers[index];
  const time_t now = time(nullptr);
  time_t newDeadline = t.deadlineEpoch + deltaSeconds;
  const time_t floor = (now < TIMER_MIN_VALID_EPOCH ? 0 : now) + 1;
  if (newDeadline < floor) {
    newDeadline = floor;
  }
  // Cap the recorded duration at the timer's lifetime so the response can
  // describe it sensibly. If shrinking past elapsed time, just leave duration
  // alone — the deadline floor above prevents negatives.
  const int64_t newDuration =
      static_cast<int64_t>(t.durationSeconds) + deltaSeconds;
  if (newDuration > 0 && newDuration <= TIMER_MAX_DURATION_SEC) {
    t.durationSeconds = static_cast<uint32_t>(newDuration);
  }
  t.deadlineEpoch = newDeadline;
  persist();

  outAdjusted = t;
  return Result::Ok;
}

int TimerService::harvestExpired(time_t now, TimerRecord *out, int maxOut) {
  if (_count == 0 || maxOut <= 0) {
    return 0;
  }
  if (now < TIMER_MIN_VALID_EPOCH) {
    return 0;
  }

  int harvested = 0;
  int i = 0;
  while (i < _count && harvested < maxOut) {
    if (_timers[i].deadlineEpoch <= now) {
      out[harvested++] = _timers[i];
      removeAt(i);
      continue;
    }
    i++;
  }
  if (harvested > 0) {
    persist();
  }
  return harvested;
}

time_t TimerService::nextDeadline() const {
  if (_count == 0) {
    return 0;
  }
  time_t earliest = _timers[0].deadlineEpoch;
  for (int i = 1; i < _count; i++) {
    if (_timers[i].deadlineEpoch < earliest) {
      earliest = _timers[i].deadlineEpoch;
    }
  }
  return earliest;
}

String TimerService::describeAll(time_t now) const {
  JsonDocument doc;
  doc["count"] = _count;
  JsonArray arr = doc["timers"].to<JsonArray>();
  for (int i = 0; i < _count; i++) {
    const TimerRecord &t = _timers[i];
    JsonObject row = arr.add<JsonObject>();
    row["id"] = t.id;
    row["name"] = t.name;
    row["duration_seconds"] = t.durationSeconds;
    const int32_t remaining =
        now < TIMER_MIN_VALID_EPOCH
            ? static_cast<int32_t>(t.durationSeconds)
            : static_cast<int32_t>(t.deadlineEpoch - now);
    row["remaining_seconds"] = remaining < 0 ? 0 : remaining;
    row["created_at_epoch"] = static_cast<uint32_t>(t.createdAtEpoch);
  }
  String out;
  serializeJson(doc, out);
  return out;
}

const char *TimerService::describeResult(Result r) {
  switch (r) {
  case Result::Ok:
    return "ok";
  case Result::ClockNotSynced:
    return "clock not synced yet — try again in a moment";
  case Result::InvalidDuration:
    return "duration out of range";
  case Result::LimitReached:
    return "timer limit reached";
  case Result::DuplicateName:
    return "a timer with that name is already running";
  case Result::NotFound:
    return "no timer with that name";
  case Result::Ambiguous:
    return "multiple timers active — call list_timers and pass the target id";
  case Result::NoTimers:
    return "no timers running";
  }
  return "error";
}

int TimerService::findByName(const String &name) const {
  for (int i = 0; i < _count; i++) {
    if (equalsIgnoreCaseTrim(_timers[i].name, name)) {
      return i;
    }
  }
  return -1;
}

int TimerService::findById(uint32_t id) const {
  if (id == 0) return -1;
  for (int i = 0; i < _count; i++) {
    if (_timers[i].id == id) return i;
  }
  return -1;
}

void TimerService::removeAt(int index) {
  if (index < 0 || index >= _count) {
    return;
  }
  for (int i = index; i < _count - 1; i++) {
    _timers[i] = _timers[i + 1];
  }
  _timers[_count - 1] = TimerRecord{};
  _count--;
}

void TimerService::persist() {
  if (!_prefsReady) return;

  JsonDocument doc;
  doc["next_id"] = _nextId;
  JsonArray arr = doc["list"].to<JsonArray>();
  for (int i = 0; i < _count; i++) {
    JsonObject row = arr.add<JsonObject>();
    row["i"] = _timers[i].id;
    row["n"] = _timers[i].name;
    row["d"] = static_cast<uint32_t>(_timers[i].deadlineEpoch);
    row["s"] = _timers[i].durationSeconds;
    row["c"] = static_cast<uint32_t>(_timers[i].createdAtEpoch);
  }
  String blob;
  serializeJson(doc, blob);
  _prefs.putString(kBlobKey, blob);
}

void TimerService::load() {
  if (!_prefsReady) return;

  const String blob = _prefs.getString(kBlobKey, "");
  if (blob.isEmpty()) {
    return;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, blob);
  if (err) {
    Serial.printf("[Timers] Could not parse stored blob: %s\n", err.c_str());
    _prefs.remove(kBlobKey);
    return;
  }

  // The blob is either the new shape ({next_id, list:[...]}) or the legacy bare
  // array of records. Detect by presence of "list".
  JsonArray arr;
  if (doc["list"].is<JsonArray>()) {
    _nextId = doc["next_id"].as<uint32_t>();
    if (_nextId == 0) _nextId = 1;
    arr = doc["list"].as<JsonArray>();
  } else {
    arr = doc.as<JsonArray>();
  }

  for (JsonObject row : arr) {
    if (_count >= MAX_TIMERS) break;
    TimerRecord &slot = _timers[_count];
    slot.id = row["i"].as<uint32_t>();
    slot.name = row["n"].as<const char *>() ? row["n"].as<const char *>() : "";
    slot.deadlineEpoch = static_cast<time_t>(row["d"].as<uint32_t>());
    slot.durationSeconds = row["s"].as<uint32_t>();
    // Backwards-compat: older blobs lack "c"; derive from deadline - duration.
    const uint32_t stored = row["c"].as<uint32_t>();
    if (stored > 0) {
      slot.createdAtEpoch = static_cast<time_t>(stored);
    } else {
      slot.createdAtEpoch =
          slot.deadlineEpoch - static_cast<time_t>(slot.durationSeconds);
    }
    if (slot.deadlineEpoch > 0 && slot.durationSeconds > 0) {
      // Backfill an id for legacy rows that never had one. nextId climbs above
      // any ids we read so monotonicity is preserved.
      if (slot.id == 0) slot.id = _nextId++;
      else if (slot.id >= _nextId) _nextId = slot.id + 1;
      _count++;
    }
  }

  Serial.printf("[Timers] Loaded %d active timer(s), next id %u\n", _count,
                static_cast<unsigned>(_nextId));
}
