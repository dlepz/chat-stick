#pragma once

#include "../Config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <functional>

/**
 * @brief Callback hooks invoked as live session events arrive from the server.
 */
struct LiveSessionCallbacks {
  /// Invoked for session or transport errors.
  std::function<void(const String &, const String &)> onError;

  /// Invoked when network activity occurs and idle timers should reset.
  std::function<void()> onActivity;

  /// Invoked when transient status text should be shown.
  std::function<void(const String &)> onStatus;

  /// Invoked once the live session is ready for use.
  std::function<void()> onReady;

  /// Invoked when Gemini finishes the current turn.
  std::function<void()> onTurnComplete;

  /// Invoked when buffered audio should be discarded.
  std::function<void()> onDropAudio;

  /// Invoked when the server assigns or changes the chat id.
  std::function<void(const String &)> onChatId;

  /// Invoked when the conversation is reset remotely.
  std::function<void()> onConversationReset;

  /// Invoked when text should be displayed on screen.
  std::function<void(const String &)> onShowText;

  /// Invoked when an image payload should be displayed.
  std::function<void(const uint8_t *, size_t, int, int)> onShowImage;

  /// Invoked while an image request is still in progress.
  std::function<void()> onShowImagePending;

  /// Invoked when an image request fails.
  std::function<void()> onShowImageFailed;

  /// Invoked for transcript updates from user or assistant.
  std::function<void(const String &, const String &)> onTranscript;

  /// Invoked when the server reports audio was ignored.
  std::function<void(const String &)> onIgnoredAudio;

  /// Invoked with PCM audio that should be queued for playback.
  std::function<void(const uint8_t *, size_t)> onAudio;

  /// Invoked when a tool requests brightness changes.
  std::function<void(int)> onBrightness;

  /// Invoked when a tool requests volume changes.
  std::function<void(int)> onVolume;

  /// Invoked when a named UI sound should be played.
  std::function<bool(const String &)> onPlaySound;

  /// Invoked when a melody sequence should be played.
  std::function<bool(const String &)> onPlayMelody;

  /// Invoked when a tool requests power-off.
  std::function<void()> onPowerOff;

  /// Invoked when power timeout settings should be updated.
  std::function<void(unsigned long, unsigned long, unsigned long,
                     unsigned long)>
      onPowerTimeouts;

  /// Called when the server requests a JSON device-status payload.
  std::function<String()> getDeviceStatusJson;

  /// Invoked when the voice selection changes remotely.
  std::function<void(const String &)> onVoiceChanged;

  /// Invoked when a server endpoint succeeds and should be preferred later.
  std::function<void(int)> onEndpointIndexChanged;
};

/**
 * @brief Summary entry returned by the conversation history endpoint.
 */
struct ConversationSummary {
  /// Conversation identifier.
  String chatId;

  /// Last assistant message preview.
  String lastMessage;

  /// Timestamp of the last update.
  String updatedAt;
};

/**
 * @brief Firmware update metadata returned by the update endpoint.
 */
struct FirmwareUpdateInfo {
  /// Whether a newer firmware build is available.
  bool available = false;

  /// Latest available firmware version number.
  int latestVersion = 0;

  /// Release notes for the available update.
  String notes;

  /// Download URL for the OTA payload.
  String downloadUrl;
};

/**
 * @brief Maintains the device-side WebSocket session with the chat server.
 */
class LiveSessionService {
public:
  /**
   * @brief Initialize the service with callback hooks.
   * @param callbacks Callback bundle for session events and tool actions.
   */
  void init(const LiveSessionCallbacks &callbacks);

  /// Open the live session WebSocket connection.
  void connect();

  /// Close the live session WebSocket connection.
  void disconnect();

  /// Pump WebSocket client work and timers.
  void poll();

  /**
   * @brief Reconnect when disconnected and reconnects are allowed.
   * @param enabled Whether reconnect behavior is currently enabled.
   */
  void reconnectIfNeeded(bool enabled);

  /**
   * @brief Update the chat id sent with subsequent requests.
   * @param chatId Conversation identifier.
   */
  void setChatId(const String &chatId) { _chatId = chatId; }

  /**
   * @brief Update the selected voice sent with subsequent requests.
   * @param voice Voice identifier.
   */
  void setVoice(const String &voice) { _voice = voice; }

  /**
   * @brief Prefer a server endpoint for the next connection attempt.
   * @param endpointIndex Index into SERVER_ENDPOINTS.
   */
  void setPreferredEndpointIndex(int endpointIndex);

  /// Current selected voice identifier.
  const String &voice() const { return _voice; }

  /// Whether the live session is currently connected.
  bool isConnected() const { return _connected; }

  /// Index of the currently connected server endpoint.
  int activeServerIndex() const { return _activeServerIndex; }

  /// Human-readable label for the active endpoint.
  String activeEndpointLabel() const;

  /// Send a start-of-turn message.
  bool sendStart();

  /// Send an end-of-turn message.
  bool sendStop();

  /**
   * @brief Send a chunk of microphone PCM data.
   * @param data Pointer to PCM samples.
   * @param len Number of bytes to send.
   * @return True when the chunk was queued to the socket.
   */
  bool sendAudio(const int16_t *data, size_t len);

  /**
   * @brief Fetch the last assistant message for session restore.
   * @param outMessage Receives the last assistant message.
   * @return True on success.
   */
  bool fetchLastAssistantMessage(String &outMessage);

  /**
   * @brief Fetch recent conversation history summaries.
   * @param outEntries Output buffer for summary entries.
   * @param maxEntries Capacity of the output buffer.
   * @param outCount Receives the number of entries written.
   * @return True on success.
   */
  bool fetchConversationHistory(ConversationSummary outEntries[],
                                int maxEntries, int &outCount);

  /**
   * @brief Check whether a newer firmware version is available.
   * @param outInfo Receives firmware update metadata.
   * @return True on successful request parsing.
   */
  bool checkFirmwareUpdate(FirmwareUpdateInfo &outInfo);

  /**
   * @brief Download and apply an OTA firmware update.
   * @param downloadUrl URL of the OTA binary.
   * @param outError Receives a human-readable error on failure.
   * @return True when the OTA process completes successfully.
   */
  bool downloadAndApplyFirmwareUpdate(const String &downloadUrl,
                                      String &outError);

private:
  /// Active WebSocket client connection to the server.
  websockets::WebsocketsClient _ws;

  /// Registered callback bundle.
  LiveSessionCallbacks _callbacks;

  /// Current conversation identifier.
  String _chatId;

  /// Current selected voice identifier.
  String _voice;

  /// Whether the WebSocket is currently connected.
  bool _connected = false;

  /// Timestamp of the last reconnect attempt.
  unsigned long _lastReconnectMs = 0;

  /// Next endpoint index to try when reconnecting.
  int _nextServerIndex = 0;

  /// Index of the currently active endpoint, or -1 when disconnected.
  int _activeServerIndex = -1;

  /// Delay between reconnect attempts.
  static constexpr unsigned long kReconnectMs = 5000;

  /// Handle an incoming WebSocket message.
  void handleMessage(websockets::WebsocketsMessage msg);

  /// Handle a WebSocket connection event.
  void handleEvent(websockets::WebsocketsEvent event, String data);

  /// Dispatch a tool call payload from the server.
  void handleToolCall(const ArduinoJson::JsonDocument &doc);

  /**
   * @brief Send a tool response back to the server.
   * @param name Tool name.
   * @param id Tool call identifier.
   * @param result Serialized tool result payload.
   */
  void sendToolResponse(const char *name, const char *id, const String &result);

  /**
   * @brief Build the HTTP base URL for a configured server endpoint.
   * @param endpoint Endpoint configuration.
   * @return Base URL string.
   */
  String endpointBaseUrl(const ServerEndpoint &endpoint) const;

  /**
   * @brief Remember the endpoint that most recently worked.
   * @param endpointIndex Index into SERVER_ENDPOINTS.
   */
  void rememberSuccessfulEndpoint(int endpointIndex);
};
