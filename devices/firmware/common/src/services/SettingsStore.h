#pragma once

#include <Arduino.h>
#include <Preferences.h>

class SettingsStore {
public:
  void init();

  int brightness() const { return _brightness; }
  int volume() const { return _volume; }
  const String &chatId() const { return _chatId; }
  bool useExternalSpeaker() const { return _useExternalSpeaker; }
  int externalSpeakerGain() const { return _externalSpeakerGain; }
  const String &voice() const { return _voice; }
  int serverEndpointIndex() const { return _serverEndpointIndex; }
  bool pendingFirmwareUpdate() const { return _pendingFirmwareUpdate; }
  int pendingFirmwareVersion() const { return _pendingFirmwareVersion; }
  const String &pendingFirmwareDownloadUrl() const {
    return _pendingFirmwareDownloadUrl;
  }

  void setBrightness(int brightness);
  void setVolume(int volume);
  void setChatId(const String &chatId);
  void clearChatId();
  void setUseExternalSpeaker(bool enabled);
  void setExternalSpeakerGain(int gain);
  void setVoice(const String &voice);
  void setServerEndpointIndex(int endpointIndex);
  void setPendingFirmwareUpdate(int version, const String &downloadUrl);
  void clearPendingFirmwareUpdate();
  void reset();

  static constexpr int kDefaultExternalGain = 24;
  static constexpr int kMinExternalGain = 1;
  static constexpr int kMaxExternalGain = 64;
  static constexpr const char *kDefaultVoice = "Aoede";

private:
  Preferences _prefs;
  int _brightness = 80;
  int _volume = 255;
  String _chatId;
  bool _useExternalSpeaker = false;
  int _externalSpeakerGain = kDefaultExternalGain;
  String _voice = kDefaultVoice;
  int _serverEndpointIndex = 0;
  bool _pendingFirmwareUpdate = false;
  int _pendingFirmwareVersion = 0;
  String _pendingFirmwareDownloadUrl;
  bool _ready = false;

  static constexpr const char *kNamespace = "chat-stick";
  static constexpr const char *kBrightnessKey = "brightness";
  static constexpr const char *kVolumeKey = "volume";
  static constexpr const char *kChatIdKey = "chat_id";
  static constexpr const char *kExternalSpeakerKey = "ext_spk";
  static constexpr const char *kExternalGainKey = "ext_gain";
  static constexpr const char *kVoiceKey = "voice";
  static constexpr const char *kServerEndpointKey = "server_idx";
  static constexpr const char *kFirmwarePendingKey = "fw_pending";
  static constexpr const char *kFirmwareVersionKey = "fw_ver";
  static constexpr const char *kFirmwareUrlKey = "fw_url";
};
