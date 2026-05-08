#pragma once

#include <Arduino.h>
#include <Preferences.h>

class SettingsStore {
public:
  void init();

  int brightness() const { return _brightness; }
  int volume() const { return _volume; }
  const String &chatId() const { return _chatId; }
  const String &voice() const { return _voice; }

  void setBrightness(int brightness);
  void setVolume(int volume);
  void setChatId(const String &chatId);
  void clearChatId();
  void setVoice(const String &voice);
  void reset();

  static constexpr const char *kDefaultVoice = "Aoede";

private:
  Preferences _prefs;
  int _brightness = 80;
  int _volume = 255;
  String _chatId;
  String _voice = kDefaultVoice;
  bool _ready = false;

  static constexpr const char *kNamespace = "chat-stick";
  static constexpr const char *kBrightnessKey = "brightness";
  static constexpr const char *kVolumeKey = "volume";
  static constexpr const char *kChatIdKey = "chat_id";
  static constexpr const char *kVoiceKey = "voice";
};
