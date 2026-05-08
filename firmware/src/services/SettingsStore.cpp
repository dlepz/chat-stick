#include "SettingsStore.h"

#include "../Config.h"
#include "../diag/Log.h"

void SettingsStore::init() {
  _brightness = DEFAULT_BRIGHTNESS;
  _volume = DEFAULT_VOLUME;
  _chatId = "";
  _voice = kDefaultVoice;

  _ready = _prefs.begin(kNamespace, false);
  if (!_ready) {
    Log::client("Settings", "preferences init failed; using defaults");
    return;
  }

  _brightness =
      constrain(_prefs.getUChar(kBrightnessKey, DEFAULT_BRIGHTNESS), 0, 255);
  _volume = DEFAULT_VOLUME;
  _prefs.putUChar(kVolumeKey, static_cast<uint8_t>(_volume));
  _chatId = _prefs.getString(kChatIdKey, "");
  _voice = _prefs.getString(kVoiceKey, kDefaultVoice);
  if (_voice.isEmpty()) {
    _voice = kDefaultVoice;
  }

  Log::client("Settings", "loaded brightness=%d volume=%d chat=%s voice=%s",
              _brightness, _volume,
              _chatId.isEmpty() ? "(none)" : _chatId.c_str(),
              _voice.c_str());
}

void SettingsStore::setBrightness(int brightness) {
  _brightness = constrain(brightness, 0, 255);
  if (_ready) {
    _prefs.putUChar(kBrightnessKey, static_cast<uint8_t>(_brightness));
  }
}

void SettingsStore::setVolume(int volume) {
  _volume = constrain(volume, 0, 255);
  if (_ready) {
    _prefs.putUChar(kVolumeKey, static_cast<uint8_t>(_volume));
  }
}

void SettingsStore::setChatId(const String &chatId) {
  _chatId = chatId;
  if (_ready) {
    _prefs.putString(kChatIdKey, _chatId);
  }
}

void SettingsStore::clearChatId() {
  _chatId = "";
  if (_ready) {
    _prefs.remove(kChatIdKey);
  }
}

void SettingsStore::setVoice(const String &voice) {
  _voice = voice.isEmpty() ? String(kDefaultVoice) : voice;
  if (_ready) {
    _prefs.putString(kVoiceKey, _voice);
  }
}

void SettingsStore::reset() {
  _brightness = DEFAULT_BRIGHTNESS;
  _volume = DEFAULT_VOLUME;
  _chatId = "";
  _voice = kDefaultVoice;

  if (_ready) {
    _prefs.clear();
  }
}
