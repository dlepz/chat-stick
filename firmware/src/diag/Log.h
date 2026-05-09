#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <time.h>

namespace Log {
using Sink = void (*)(void *ctx, char side, const char *topic,
                      const char *message);

namespace detail {
struct SinkState {
  Sink fn;
  void *ctx;
};

inline SinkState &sinkState() {
  static SinkState s = {nullptr, nullptr};
  return s;
}

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

inline void setSink(Sink sink, void *ctx) {
  detail::SinkState &state = detail::sinkState();
  state.fn = sink;
  state.ctx = ctx;
}

inline void clearSink() {
  detail::SinkState &state = detail::sinkState();
  state.fn = nullptr;
  state.ctx = nullptr;
}

inline void client(const char *topic, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  detail::write('C', topic, fmt, args);
  va_end(args);
}

inline void server(const char *topic, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  detail::write('S', topic, fmt, args);
  va_end(args);
}
} // namespace Log
