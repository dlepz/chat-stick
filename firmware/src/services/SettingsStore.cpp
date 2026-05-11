#include "SettingsStore.h"

#include "../Config.h"
#include "../diag/Log.h"

/**
 * @brief Open NVS preferences and load persisted device settings.
 */
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

/**
 * @brief Persist a new screen brightness level.
 * @param brightness Brightness level to save.
 */
void SettingsStore::setBrightness(int brightness) {
  _brightness = constrain(brightness, 0, 255);
  if (_ready) {
    _prefs.putUChar(kBrightnessKey, static_cast<uint8_t>(_brightness));
  }
}

/**
 * @brief Persist a new speaker volume level.
 * @param volume Volume level to save.
 */
void SettingsStore::setVolume(int volume) {
  _volume = constrain(volume, 0, 255);
  if (_ready) {
    _prefs.putUChar(kVolumeKey, static_cast<uint8_t>(_volume));
  }
}

/**
 * @brief Persist the active conversation id.
 * @param chatId Conversation identifier to save.
 */
void SettingsStore::setChatId(const String &chatId) {
  _chatId = chatId;
  if (_ready) {
    _prefs.putString(kChatIdKey, _chatId);
  }
}

/**
 * @brief Remove any persisted conversation id.
 */
void SettingsStore::clearChatId() {
  _chatId = "";
  if (_ready) {
    _prefs.remove(kChatIdKey);
  }
}

/**
 * @brief Persist the selected voice identifier.
 * @param voice Voice identifier to save; empty values fall back to the default.
 */
void SettingsStore::setVoice(const String &voice) {
  _voice = voice.isEmpty() ? String(kDefaultVoice) : voice;
  if (_ready) {
    _prefs.putString(kVoiceKey, _voice);
  }
}

/**
 * @brief Persist the preferred server endpoint index.
 * @param endpointIndex Index into SERVER_ENDPOINTS to save.
 */
void SettingsStore::setServerEndpointIndex(int endpointIndex) {
  _serverEndpointIndex = max(0, endpointIndex);
  if (_ready) {
    _prefs.putInt(kServerEndpointKey, _serverEndpointIndex);
  }
}

/**
 * @brief Clear persisted settings and restore in-memory defaults.
 */
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
