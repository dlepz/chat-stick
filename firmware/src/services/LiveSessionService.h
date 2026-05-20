#pragma once

#include "../Config.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <functional>

struct LiveSessionCallbacks {
  std::function<void(const String &, const String &)> onError;
  std::function<void()> onActivity;
  std::function<void(const String &)> onStatus;
  std::function<void()> onServerReady;
  std::function<void()> onReady;
  std::function<void()> onTurnComplete;
  std::function<void()> onDropAudio;
  std::function<void(const String &)> onChatId;
  std::function<void()> onConversationReset;
  std::function<void(const String &)> onShowText;
  std::function<void(const uint8_t *, size_t, int, int)> onShowImage;
  std::function<void()> onShowImagePending;
  std::function<void()> onShowImageFailed;
  std::function<void(const String &, const String &)> onTranscript;
  std::function<void(const String &)> onIgnoredAudio;
  std::function<void(const uint8_t *, size_t)> onAudio;
  std::function<void(int)> onBrightness;
  std::function<void(int)> onVolume;
  std::function<bool(const String &)> onSetSpeaker;
  std::function<bool(int)> onSetExternalGain;
  std::function<bool(const String &)> onPlaySound;
  std::function<bool(const String &)> onPlayMelody;
  std::function<void()> onPowerOff;
  std::function<String()> getDeviceStatusJson;
  std::function<void(const String &)> onVoiceChanged;
};

struct ConversationSummary {
  String chatId;
  String lastMessage;
  String updatedAt;
};

struct FirmwareUpdateInfo {
  bool available = false;
  int latestVersion = 0;
  String notes;
  String downloadUrl;
};

class LiveSessionService {
public:
  void init(const LiveSessionCallbacks &callbacks);
  void connect();
  void disconnect();
  void poll();
  void reconnectIfNeeded(bool enabled);
  void setChatId(const String &chatId) { _chatId = chatId; }
  void setVoice(const String &voice) { _voice = voice; }
  const String &voice() const { return _voice; }

  bool isConnected() const { return _connected; }
  int activeServerIndex() const { return _activeServerIndex; }
  String activeEndpointLabel() const;

  bool pingServer();
  bool sendStart();
  bool sendStop();
  bool sendAudio(const int16_t *data, size_t len);
  bool fetchConversationHistory(ConversationSummary outEntries[], int maxEntries,
                                int &outCount);
  bool checkFirmwareUpdate(FirmwareUpdateInfo &outInfo);
  bool downloadAndApplyFirmwareUpdate(const String &downloadUrl,
                                      String &outError);

private:
  websockets::WebsocketsClient _ws;
  Preferences _prefs;
  LiveSessionCallbacks _callbacks;
  String _chatId;
  String _voice;
  bool _connected = false;
  bool _prefsReady = false;
  unsigned long _lastReconnectMs = 0;
  int _nextServerIndex = 0;
  int _activeServerIndex = -1;

  static constexpr unsigned long kReconnectMs = 5000;
  static constexpr const char *kPrefsNamespace = "live";
  static constexpr const char *kLastServerIndexKey = "server";

  void handleMessage(websockets::WebsocketsMessage msg);
  void handleEvent(websockets::WebsocketsEvent event, String data);
  void handleToolCall(const ArduinoJson::JsonDocument &doc);
  void sendToolResponse(const char *name, const char *id, const String &result);
  String endpointBaseUrl(const ServerEndpoint &endpoint) const;
  void rememberSuccessfulEndpoint(int index);
};
