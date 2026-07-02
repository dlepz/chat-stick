#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <time.h>

/**
 * @brief Lightweight logging helpers with an optional mirrored sink callback.
 */
namespace Log {
/// Signature for an optional secondary log sink.
using Sink = void (*)(void *ctx, char side, const char *topic,
                      const char *message);

namespace detail {
/**
 * @brief Stores the currently registered sink callback.
 */
struct SinkState {
  /// Sink callback function.
  Sink fn;

  /// Opaque sink callback context pointer.
  void *ctx;
};

/// Access the process-wide sink registration state.
inline SinkState &sinkState() {
  static SinkState s = {nullptr, nullptr};
  return s;
}

/// Format the current time into a short log timestamp.
inline void timestamp(char *out, size_t len) {
  time_t now = time(nullptr);
  struct tm local;
  if (localtime_r(&now, &local) && local.tm_year + 1900 >= 2024) {
    strftime(out, len, "%H:%M:%S", &local);
    return;
  }

  const unsigned long seconds = millis() / 1000;
  snprintf(out, len, "%02lu:%02lu:%02lu", (seconds / 3600) % 24,
           (seconds / 60) % 60, seconds % 60);
}

/// Write one formatted log line to Serial and the optional sink.
inline void write(char side, const char *topic, const char *fmt, va_list args) {
  char ts[9];
  char message[512];
  timestamp(ts, sizeof(ts));
  vsnprintf(message, sizeof(message), fmt, args);
  Serial.printf("%s - %c - %s - %s\n", ts, side, topic, message);
  SinkState &state = sinkState();
  if (state.fn) {
    state.fn(state.ctx, side, topic, message);
  }
}
} // namespace detail

/**
 * @brief Register a mirrored log sink.
 * @param sink Sink callback function.
 * @param ctx Opaque callback context.
 */
inline void setSink(Sink sink, void *ctx) {
  detail::SinkState &state = detail::sinkState();
  state.fn = sink;
  state.ctx = ctx;
}

/// Remove any registered mirrored log sink.
inline void clearSink() {
  detail::SinkState &state = detail::sinkState();
  state.fn = nullptr;
  state.ctx = nullptr;
}

/**
 * @brief Emit a client-side log line.
 * @param topic Log topic.
 * @param fmt printf-style format string.
 */
inline void client(const char *topic, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  detail::write('C', topic, fmt, args);
  va_end(args);
}

/**
 * @brief Emit a server-side log line.
 * @param topic Log topic.
 * @param fmt printf-style format string.
 */
inline void server(const char *topic, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  detail::write('S', topic, fmt, args);
  va_end(args);
}
} // namespace Log
