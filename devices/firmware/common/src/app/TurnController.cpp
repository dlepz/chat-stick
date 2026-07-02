#include "TurnController.h"

namespace {
/**
 * @brief Check whether the current text ends with a prefix of incoming text.
 * @param current Text already shown.
 * @param incoming Newly received text.
 * @param prefixLen Prefix length from @p incoming to compare.
 * @return True when the suffix/prefix overlap matches.
 */
bool endsWithIncomingPrefix(const String &current, const String &incoming,
                            int prefixLen) {
  if (prefixLen <= 0 || prefixLen > current.length() ||
      prefixLen > incoming.length()) {
    return false;
  }

  const int currentStart = current.length() - prefixLen;
  for (int i = 0; i < prefixLen; i++) {
    if (current.charAt(currentStart + i) != incoming.charAt(i)) {
      return false;
    }
  }
  return true;
}
} // namespace

void TurnController::beginRecording(unsigned long nowMs) {
  _complete = false;
  _hasAudio = false;
  _hasResponseContent = false;
  _audioChunksSent = 0;
  _audioChunksFailed = 0;
  _captureFailures = 0;
  _audioBytesSent = 0;
  _lastCaptureFailureLogMs = 0;
  _recordingStartMs = nowMs;
}

void TurnController::beginThinking(unsigned long nowMs) {
  _pendingReset = true;
  _thinkingStartMs = nowMs;
}

unsigned long TurnController::recordingDurationMs(unsigned long nowMs) const {
  return nowMs - _recordingStartMs;
}

bool TurnController::recordingTimedOut(unsigned long nowMs,
                                       unsigned long maxMs) const {
  return nowMs - _recordingStartMs >= maxMs;
}

bool TurnController::thinkingTimedOut(unsigned long nowMs,
                                      unsigned long maxMs) const {
  return nowMs - _thinkingStartMs > maxMs;
}

bool TurnController::noteTurnComplete() {
  if (!_hasAudio && !_hasResponseContent) {
    return false;
  }
  _complete = true;
  return true;
}

void TurnController::noteAudioReceived() {
  _hasAudio = true;
  _hasResponseContent = true;
}

void TurnController::noteResponseContent() { _hasResponseContent = true; }

void TurnController::clearResponse() {
  _complete = false;
  _hasAudio = false;
  _hasResponseContent = false;
}

void TurnController::clearPendingReset() { _pendingReset = false; }

TurnController::CaptureFailureReport
TurnController::noteCaptureFailure(unsigned long nowMs,
                                   unsigned long logIntervalMs) {
  _captureFailures++;
  const bool shouldLog =
      _captureFailures <= 3 ||
      nowMs - _lastCaptureFailureLogMs >= logIntervalMs;
  if (shouldLog) {
    _lastCaptureFailureLogMs = nowMs;
  }

  CaptureFailureReport report;
  report.count = _captureFailures;
  report.shouldLog = shouldLog;
  report.firstFailure = _captureFailures == 1;
  return report;
}

int TurnController::noteAudioSendFailed() { return ++_audioChunksFailed; }

int TurnController::noteAudioChunkSent(size_t bytes) {
  _audioChunksSent++;
  _audioBytesSent += bytes;
  return _audioChunksSent;
}

String TurnController::transcriptDelta(const String &current,
                                       const String &incoming) const {
  if (incoming.isEmpty()) {
    return "";
  }
  if (current.isEmpty()) {
    return incoming;
  }

  // Gemini Live transcription usually streams deltas, but early partial
  // hypotheses can repeat as cumulative or overlapping text. Keep the on-screen
  // text append-only so repeated partials do not duplicate words.
  if (incoming == current || current.startsWith(incoming)) {
    return "";
  }
  if (incoming.startsWith(current)) {
    return incoming.substring(current.length());
  }
  if (current.endsWith(incoming)) {
    return "";
  }

  const int maxOverlap = min(current.length(), incoming.length());
  for (int len = maxOverlap; len >= 1; len--) {
    if (endsWithIncomingPrefix(current, incoming, len)) {
      return incoming.substring(len);
    }
  }

  return incoming;
}
