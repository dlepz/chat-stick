#include "SettingsStore.h"

#include "../Config.h"
#include "../diag/Log.h"

void SettingsStore::init() {
  _brightness = DEFAULT_BRIGHTNESS;
  _volume = DEFAULT_VOLUME;
  _chatId = "";
  _voice = kDefaultVoice;
  _serverEndpointIndex = 0;

  _ready = _prefs.begin(kNamespace, false);
  if (!_ready) {
    Log::client("Settings", "preferences init failed; using defaults");
    return;
  }

  _brightness =
      constrain(_prefs.getUChar(kBrightnessKey, DEFAULT_BRIGHTNESS), 0, 255);
  _volume = constrain(_prefs.getUChar(kVolumeKey, DEFAULT_VOLUME), 0, 255);
  _chatId = _prefs.getString(kChatIdKey, "");
  _voice = _prefs.getString(kVoiceKey, kDefaultVoice);
  _serverEndpointIndex =
      max(0, static_cast<int>(_prefs.getInt(kServerEndpointKey, 0)));
  if (_voice.isEmpty()) {
    _voice = kDefaultVoice;
  }

  Log::client("Settings",
              "loaded brightness=%d volume=%d chat=%s voice=%s server=%d",
              _brightness, _volume,
              _chatId.isEmpty() ? "(none)" : _chatId.c_str(),
              _voice.c_str(), _serverEndpointIndex);
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

void SettingsStore::setServerEndpointIndex(int endpointIndex) {
  _serverEndpointIndex = max(0, endpointIndex);
  if (_ready) {
    _prefs.putInt(kServerEndpointKey, _serverEndpointIndex);
  }
}

void SettingsStore::reset() {
  _brightness = DEFAULT_BRIGHTNESS;
  _volume = DEFAULT_VOLUME;
  _chatId = "";
  _voice = kDefaultVoice;
  _serverEndpointIndex = 0;

  if (_ready) {
    _prefs.clear();
  }
}
