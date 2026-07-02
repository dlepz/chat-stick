#pragma once

#include <Arduino.h>

/**
 * @brief Per-turn voice exchange state and metrics.
 */
class TurnController {
public:
  /**
   * @brief Result of recording a microphone capture failure.
   */
  struct CaptureFailureReport {
    /// Total failures observed during the current recording turn.
    int count = 0;

    /// Whether this failure should be logged after rate limiting.
    bool shouldLog = false;

    /// Whether this is the first capture failure in the turn.
    bool firstFailure = false;
  };

  /// Reset response flags and recording metrics at the start of capture.
  void beginRecording(unsigned long nowMs);

  /// Mark recording complete and arm UI cleanup for the incoming response.
  void beginThinking(unsigned long nowMs);

  /// Duration of the active recording turn.
  unsigned long recordingDurationMs(unsigned long nowMs) const;

  /// Whether the active recording turn exceeded the configured duration.
  bool recordingTimedOut(unsigned long nowMs, unsigned long maxMs) const;

  /// Whether the active thinking turn exceeded the configured duration.
  bool thinkingTimedOut(unsigned long nowMs, unsigned long maxMs) const;

  /// Accept a remote turn-complete signal only after response audio arrives.
  bool noteTurnComplete();

  /// Mark that response audio has arrived for this turn.
  void noteAudioReceived();

  /// Mark that text, image, or tool content has arrived for this turn.
  void noteResponseContent();

  /// Clear response flags for interrupted, ignored, or completed playback.
  void clearResponse();

  /// Whether the remote side has completed the current turn.
  bool complete() const { return _complete; }

  /// Whether the current turn has produced response audio.
  bool hasAudio() const { return _hasAudio; }

  /// Whether the current turn has produced any visible/audible content.
  bool hasResponseContent() const { return _hasResponseContent; }

  /// Cancel delayed cleanup of the previous tool/image body.
  void clearPendingReset();

  /// Whether delayed cleanup is waiting for first response content.
  bool pendingReset() const { return _pendingReset; }

  /// Record a microphone capture failure and return rate-limit metadata.
  CaptureFailureReport noteCaptureFailure(unsigned long nowMs,
                                          unsigned long logIntervalMs);

  /// Record a failed audio chunk send and return the failure count.
  int noteAudioSendFailed();

  /// Record a sent audio chunk and return the sent chunk count.
  int noteAudioChunkSent(size_t bytes);

  /// Number of successfully sent audio chunks.
  int audioChunksSent() const { return _audioChunksSent; }

  /// Number of failed audio chunk sends.
  int audioChunksFailed() const { return _audioChunksFailed; }

  /// Number of capture failures in the current turn.
  int captureFailures() const { return _captureFailures; }

  /// Total microphone bytes sent in the current turn.
  size_t audioBytesSent() const { return _audioBytesSent; }

  /// Return only the not-yet-rendered suffix from a streamed transcript chunk.
  String transcriptDelta(const String &current, const String &incoming) const;

private:
  /// Whether the current assistant turn has completed.
  bool _complete = false;

  /// Whether the current assistant turn has produced audio.
  bool _hasAudio = false;

  /// Whether the current turn has produced text, image, tool, or audio content.
  bool _hasResponseContent = false;

  /// Whether turn-completion cleanup is deferred until response content arrives.
  bool _pendingReset = false;

  /// Timestamp when the current Thinking state began.
  unsigned long _thinkingStartMs = 0;

  /// Timestamp when the current recording state began.
  unsigned long _recordingStartMs = 0;

  /// Timestamp of the last capture-failure log.
  unsigned long _lastCaptureFailureLogMs = 0;

  /// Number of audio chunks sent during the current turn.
  int _audioChunksSent = 0;

  /// Number of failed audio chunk sends during the current turn.
  int _audioChunksFailed = 0;

  /// Number of recent microphone capture failures.
  int _captureFailures = 0;

  /// Total audio bytes sent during the current turn.
  size_t _audioBytesSent = 0;
};
