#pragma once

#include <Arduino.h>
#include <Preferences.h>

/**
 * @brief Persists user-configurable device settings in non-volatile storage.
 */
class SettingsStore {
public:
  /// Open preferences storage and load persisted values.
  void init();

  /// Current saved screen brightness.
  int brightness() const { return _brightness; }

  /// Current saved speaker volume.
  int volume() const { return _volume; }

  /// Active conversation id used for session restore.
  const String &chatId() const { return _chatId; }

  /// Current selected voice identifier.
  const String &voice() const { return _voice; }

  /// Preferred server endpoint index for reconnects after reboot.
  int serverEndpointIndex() const { return _serverEndpointIndex; }

  /**
   * @brief Persist a new brightness value.
   * @param brightness Brightness level to save.
   */
  void setBrightness(int brightness);

  /**
   * @brief Persist a new speaker volume value.
   * @param volume Volume level to save.
   */
  void setVolume(int volume);

  /**
   * @brief Persist the active conversation id.
   * @param chatId Conversation identifier.
   */
  void setChatId(const String &chatId);

  /// Remove any persisted conversation id.
  void clearChatId();

  /**
   * @brief Persist the selected voice identifier.
   * @param voice Voice name to save.
   */
  void setVoice(const String &voice);

  /**
   * @brief Persist the preferred server endpoint index.
   * @param endpointIndex Index into SERVER_ENDPOINTS.
   */
  void setServerEndpointIndex(int endpointIndex);

  /// Clear persisted settings and restore in-memory defaults.
  void reset();

  /// Default Gemini voice used when nothing is persisted yet.
  static constexpr const char *kDefaultVoice = "Aoede";

private:
  /// Preferences handle for the chat-stick namespace.
  Preferences _prefs;

  /// Cached brightness value.
  int _brightness = 80;

  /// Cached speaker volume value.
  int _volume = 255;

  /// Cached active conversation id.
  String _chatId;

  /// Cached current voice selection.
  String _voice = kDefaultVoice;

  /// Cached preferred server endpoint index.
  int _serverEndpointIndex = 0;

  /// Whether preferences storage has been initialized.
  bool _ready = false;

  /// Preferences namespace used for all settings keys.
  static constexpr const char *kNamespace = "chat-stick";

  /// Preferences key for screen brightness.
  static constexpr const char *kBrightnessKey = "brightness";

  /// Preferences key for speaker volume.
  static constexpr const char *kVolumeKey = "volume";

  /// Preferences key for the active conversation id.
  static constexpr const char *kChatIdKey = "chat_id";

  /// Preferences key for the selected voice.
  static constexpr const char *kVoiceKey = "voice";

  /// Preferences key for the preferred server endpoint index.
  static constexpr const char *kServerEndpointKey = "server_idx";
};
