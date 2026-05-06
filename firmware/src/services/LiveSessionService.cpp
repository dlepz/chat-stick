#include "LiveSessionService.h"

#include "../credentials.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

using namespace websockets;

namespace {
bool urlUsesHttps(const String &url) { return url.startsWith("https://"); }

String stripEndpointScheme(const char *rawHost) {
  String host = rawHost ? String(rawHost) : String("");
  if (host.startsWith("http://")) {
    host = host.substring(7);
  } else if (host.startsWith("https://")) {
    host = host.substring(8);
  } else if (host.startsWith("ws://")) {
    host = host.substring(5);
  } else if (host.startsWith("wss://")) {
    host = host.substring(6);
  }

  const int pathIndex = host.indexOf('/');
  if (pathIndex >= 0) {
    host = host.substring(0, pathIndex);
  }
  return host;
}

String endpointHostForConnection(const ServerEndpoint &endpoint) {
  String host = stripEndpointScheme(endpoint.host);
  const int colonIndex = host.lastIndexOf(':');
  if (colonIndex > 0 && host.indexOf(':') == colonIndex) {
    bool portIsNumeric = true;
    for (int i = colonIndex + 1; i < host.length(); i++) {
      if (!isDigit(host.charAt(i))) {
        portIsNumeric = false;
        break;
      }
    }
    if (portIsNumeric) {
      host = host.substring(0, colonIndex);
    }
  }
  return host;
}

const char *httpSchemeForEndpoint(const ServerEndpoint &endpoint) {
  const String rawHost = endpoint.host ? String(endpoint.host) : String("");
  if (rawHost.startsWith("https://")) {
    return "https";
  }
  if (rawHost.startsWith("http://")) {
    return "http";
  }
  return endpoint.port == 443 ? "https" : "http";
}

const char *wsSchemeForEndpoint(const ServerEndpoint &endpoint) {
  return strcmp(httpSchemeForEndpoint(endpoint), "https") == 0 ? "wss" : "ws";
}

const char *caCertForUrl(const String &url) {
  for (int i = 0; i < SERVER_ENDPOINT_COUNT; i++) {
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[i];
    const String httpsPrefix =
        String("https://") + endpointHostForConnection(endpoint);
    if (url == httpsPrefix || url.startsWith(httpsPrefix + "/") ||
        url.startsWith(httpsPrefix + ":")) {
      return endpoint.ca_cert;
    }
  }
  return nullptr;
}

String updateError() {
  const char *error = Update.errorString();
  return error && error[0] ? String(error) : String("unknown update error");
}

bool runFirmwareUpdateDownload(HTTPClient &http, String &outError) {
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    outError = "Download failed: HTTP " + String(statusCode);
    Serial.printf("[OTA] Download failed: status=%d\n", statusCode);
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    outError = "Missing firmware size";
    Serial.printf("[OTA] Missing content length: %d\n", contentLength);
    return false;
  }

  Serial.printf("[OTA] Downloading firmware: %d bytes\n", contentLength);
  if (!Update.begin(contentLength, U_FLASH)) {
    outError = "Update begin failed: " + updateError();
    Serial.printf("[OTA] Update.begin failed: %s\n", outError.c_str());
    return false;
  }

  Stream *stream = http.getStreamPtr();
  const size_t written = Update.writeStream(*stream);
  if (written != static_cast<size_t>(contentLength)) {
    outError = "Write failed: " + String(written) + "/" + String(contentLength);
    const String error = updateError();
    if (error.length()) {
      outError += " " + error;
    }
    Serial.printf("[OTA] Update.writeStream failed: wrote=%u expected=%d err=%s\n",
                  static_cast<unsigned>(written), contentLength,
                  updateError().c_str());
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    outError = "Update end failed: " + updateError();
    Serial.printf("[OTA] Update.end failed: %s\n", outError.c_str());
    return false;
  }

  if (!Update.isFinished()) {
    outError = "Update incomplete";
    Serial.println("[OTA] Update incomplete");
    return false;
  }

  Serial.println("[OTA] Firmware update installed");
  return true;
}
} // namespace

void LiveSessionService::init(const LiveSessionCallbacks &callbacks) {
  _callbacks = callbacks;
  _ws.onMessage([this](WebsocketsMessage msg) { handleMessage(msg); });
  _ws.onEvent(
      [this](WebsocketsEvent event, String data) { handleEvent(event, data); });
}

void LiveSessionService::connect() {
  _activeServerIndex = _nextServerIndex;
  const ServerEndpoint &endpoint = SERVER_ENDPOINTS[_nextServerIndex];
  _nextServerIndex = (_nextServerIndex + 1) % SERVER_ENDPOINT_COUNT;

  const String path = String(SERVER_PATH) + "?device_id=" + DEVICE_ID;
  const String chatQuery = _chatId.isEmpty() ? "" : "&chat_id=" + _chatId;
  const String voiceQuery = _voice.isEmpty() ? "" : "&voice=" + _voice;
  const String fullPath = path + chatQuery + voiceQuery;
  const String host = endpointHostForConnection(endpoint);
  const char *scheme = wsSchemeForEndpoint(endpoint);

  if (_callbacks.onStatus) {
    _callbacks.onStatus("Connecting...");
  }

  Serial.printf("[WS] Connecting to %s://%s:%d%s\n", scheme, host.c_str(),
                endpoint.port, fullPath.c_str());

  if (endpoint.ca_cert) {
    _ws.setCACert(endpoint.ca_cert);
  } else {
    _ws.setInsecure();
  }

  if (endpoint.port == 443) {
    _connected =
        _ws.connectSecure(host.c_str(), endpoint.port, fullPath.c_str());
  } else {
    _connected = _ws.connect(host.c_str(), endpoint.port, fullPath.c_str());
  }

  if (!_connected) {
    Serial.printf("[WS] Connect failed: %s://%s:%d — will retry\n", scheme,
                  host.c_str(), endpoint.port);
  }
}

void LiveSessionService::disconnect() {
  if (_ws.available()) {
    _ws.close();
  }
  _connected = false;
}

void LiveSessionService::poll() {
  if (_connected) {
    _ws.poll();
  }
}

void LiveSessionService::reconnectIfNeeded(bool enabled) {
  if (_connected || !enabled) {
    return;
  }

  const unsigned long now = millis();
  if (now - _lastReconnectMs < kReconnectMs) {
    return;
  }

  _lastReconnectMs = now;
  connect();
}

String LiveSessionService::activeEndpointLabel() const {
  if (_activeServerIndex < 0 || _activeServerIndex >= SERVER_ENDPOINT_COUNT) {
    return "no server";
  }
  // Index 0 is the local dev worker; anything else is treated as prod.
  return _activeServerIndex == 0 ? "dev" : "prod";
}

bool LiveSessionService::sendStart() { return _ws.send("{\"type\":\"start\"}"); }

bool LiveSessionService::sendStop() { return _ws.send("{\"type\":\"stop\"}"); }

bool LiveSessionService::sendAudio(const int16_t *data, size_t len) {
  return _ws.sendBinary(reinterpret_cast<const char *>(data), len);
}

bool LiveSessionService::fetchLastAssistantMessage(String &outMessage) {
  outMessage = "";
  if (_chatId.isEmpty()) {
    return false;
  }

  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url = endpointBaseUrl(endpoint) + "/session/" + _chatId +
                       "?device_id=" + DEVICE_ID;

    Serial.printf("[HTTP] Restoring session from %s\n", url.c_str());

    int statusCode = -1;
    String body;
    {
      HTTPClient http;
      if (endpoint.port == 443) {
        WiFiClientSecure client;
        if (endpoint.ca_cert) {
          client.setCACert(endpoint.ca_cert);
        } else {
          client.setInsecure();
        }
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      } else {
        WiFiClient client;
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      }
    }

    if (statusCode == 404) {
      return false;
    }

    if (statusCode != 200 || body.isEmpty()) {
      Serial.printf("[HTTP] Session restore failed: status=%d\n", statusCode);
      continue;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      Serial.println("[HTTP] Session restore returned invalid JSON");
      continue;
    }

    const char *lastMessage = doc["last_message"];
    if (!lastMessage || !lastMessage[0]) {
      return false;
    }

    outMessage = lastMessage;
    return true;
  }

  return false;
}

bool LiveSessionService::fetchConversationHistory(ConversationSummary outEntries[],
                                                  int maxEntries,
                                                  int &outCount) {
  outCount = 0;
  if (maxEntries <= 0) {
    return false;
  }

  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url = endpointBaseUrl(endpoint) + "/history/" + DEVICE_ID +
                       "?device_id=" + DEVICE_ID;

    Serial.printf("[HTTP] Fetching history from %s\n", url.c_str());

    int statusCode = -1;
    String body;
    {
      HTTPClient http;
      if (endpoint.port == 443) {
        WiFiClientSecure client;
        if (endpoint.ca_cert) {
          client.setCACert(endpoint.ca_cert);
        } else {
          client.setInsecure();
        }
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      } else {
        WiFiClient client;
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      }
    }

    if (statusCode != 200 || body.isEmpty()) {
      Serial.printf("[HTTP] History fetch failed: status=%d\n", statusCode);
      continue;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body) || !doc.is<JsonArray>()) {
      Serial.println("[HTTP] History response invalid");
      continue;
    }

    JsonArray rows = doc.as<JsonArray>();
    for (JsonVariant row : rows) {
      if (outCount >= maxEntries) {
        break;
      }

      const char *chatId = row["chat_id"];
      if (!chatId || !chatId[0]) {
        continue;
      }

      outEntries[outCount].chatId = chatId;
      outEntries[outCount].lastMessage = row["last_message"] | "";
      outEntries[outCount].updatedAt = row["updated_at"] | "";
      outCount++;
    }
    return true;
  }

  return false;
}

bool LiveSessionService::checkFirmwareUpdate(FirmwareUpdateInfo &outInfo) {
  outInfo = FirmwareUpdateInfo{};

  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url = endpointBaseUrl(endpoint) + "/firmware/check?version=" +
                       String(FIRMWARE_VERSION);

    Serial.printf("[HTTP] Checking firmware at %s\n", url.c_str());

    int statusCode = -1;
    String body;
    {
      HTTPClient http;
      if (endpoint.port == 443) {
        WiFiClientSecure client;
        if (endpoint.ca_cert) {
          client.setCACert(endpoint.ca_cert);
        } else {
          client.setInsecure();
        }
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      } else {
        WiFiClient client;
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      }
    }

    if (statusCode == 404) {
      continue;
    }

    if (statusCode != 200 || body.isEmpty()) {
      Serial.printf("[HTTP] Firmware check failed: status=%d\n", statusCode);
      continue;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      Serial.println("[HTTP] Firmware check invalid JSON");
      continue;
    }

    outInfo.available = doc["available"] | false;
    outInfo.latestVersion = doc["latest_version"] | FIRMWARE_VERSION;
    outInfo.notes = doc["notes"] | "";
    outInfo.downloadUrl = doc["download_url"] | "";
    return true;
  }

  return false;
}

bool LiveSessionService::downloadAndApplyFirmwareUpdate(
    const String &downloadUrl, String &outError) {
  outError = "";

  if (downloadUrl.isEmpty()) {
    outError = "No download URL";
    return false;
  }

  Serial.printf("[OTA] Downloading update from %s\n", downloadUrl.c_str());
  disconnect();
  delay(100);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(60000);

  if (urlUsesHttps(downloadUrl)) {
    WiFiClientSecure client;
    const char *caCert = caCertForUrl(downloadUrl);
    if (caCert) {
      client.setCACert(caCert);
    } else {
      client.setInsecure();
    }

    if (!http.begin(client, downloadUrl)) {
      outError = "Could not start download";
      return false;
    }

    const bool ok = runFirmwareUpdateDownload(http, outError);
    http.end();
    return ok;
  }

  WiFiClient client;
  if (!http.begin(client, downloadUrl)) {
    outError = "Could not start download";
    return false;
  }

  const bool ok = runFirmwareUpdateDownload(http, outError);
  http.end();
  return ok;
}

void LiveSessionService::handleEvent(WebsocketsEvent event, String data) {
  switch (event) {
  case WebsocketsEvent::ConnectionOpened:
    Serial.println("[WS] Opened");
    _connected = true;
    if (_callbacks.onStatus) {
      _callbacks.onStatus("Waiting for AI...");
    }
    break;

  case WebsocketsEvent::ConnectionClosed:
    Serial.printf("[WS] Closed: %s\n", data.c_str());
    _connected = false;
    if (_callbacks.onStatus) {
      _callbacks.onStatus("Reconnecting...");
    }
    break;

  case WebsocketsEvent::GotPing:
  case WebsocketsEvent::GotPong:
    break;
  }
}

void LiveSessionService::handleMessage(WebsocketsMessage msg) {
  if (_callbacks.onActivity) {
    _callbacks.onActivity();
  }

  if (msg.isBinary()) {
    const auto raw = msg.rawData();
    if (raw.length() >= 16 && _callbacks.onAudio) {
      _callbacks.onAudio(reinterpret_cast<const uint8_t *>(raw.c_str()),
                         raw.length());
    }
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg.data())) {
    return;
  }

  const char *type = doc["type"];
  if (!type) {
    return;
  }

  if (strcmp(type, "session") == 0) {
    const char *id = doc["chatId"];
    if (id && _callbacks.onChatId) {
      _callbacks.onChatId(id);
    }
    return;
  }

  if (strcmp(type, "ready") == 0) {
    Serial.println("[Server] Gemini session ready");
    if (_callbacks.onReady) {
      _callbacks.onReady();
    }
    return;
  }

  if (strcmp(type, "turn_complete") == 0) {
    Serial.println("[Server] Turn complete");
    if (_callbacks.onTurnComplete) {
      _callbacks.onTurnComplete();
    }
    return;
  }

  if (strcmp(type, "drop_audio") == 0) {
    Serial.println("[Server] Drop audio (interrupted)");
    if (_callbacks.onDropAudio) {
      _callbacks.onDropAudio();
    }
    return;
  }

  if (strcmp(type, "settings") == 0) {
    if (_callbacks.onPowerTimeouts) {
      _callbacks.onPowerTimeouts(doc["power"]["dim_ms"] | IDLE_DIM_MS,
                                 doc["power"]["screen_off_ms"] |
                                     IDLE_SCREEN_OFF_MS,
                                 doc["power"]["light_sleep_ms"] |
                                     IDLE_LIGHT_SLEEP_MS,
                                 doc["power"]["power_off_ms"] |
                                     IDLE_POWER_OFF_MS);
    }
    return;
  }

  if (strcmp(type, "tool_call") == 0) {
    handleToolCall(doc);
    return;
  }

  if (strcmp(type, "transcript") == 0) {
    const char *source = doc["source"];
    const char *text = doc["text"];
    Serial.printf("[Transcript] %s: %s\n", source ? source : "?",
                  text ? text : "");
    if (_callbacks.onTranscript && source && text) {
      _callbacks.onTranscript(String(source), String(text));
    }
    return;
  }

  if (strcmp(type, "error") == 0) {
    const char *category = doc["category"];
    const char *message = doc["message"];
    Serial.printf("[Server] Error: [%s] %s\n", category ? category : "server",
                  message ? message : "unknown");
    if (_callbacks.onError) {
      _callbacks.onError(category ? category : "server",
                         message ? message : "Server error");
    }
    return;
  }

  if (strcmp(type, "ignore_audio") == 0) {
    const char *reason = doc["reason"];
    if (_callbacks.onIgnoredAudio) {
      _callbacks.onIgnoredAudio(reason ? reason : "ignored");
    }
    return;
  }

  if (strcmp(type, "voice_changed") == 0) {
    const char *voice = doc["voice"];
    if (voice && *voice) {
      _voice = voice;
      Serial.printf("[Server] Voice changed to %s\n", voice);
      if (_callbacks.onVoiceChanged) {
        _callbacks.onVoiceChanged(String(voice));
      }
    }
  }
}

void LiveSessionService::handleToolCall(const JsonDocument &doc) {
  const char *name = doc["name"];
  const char *id = doc["id"];
  if (!name || !id) {
    return;
  }

  String result = "ok";

  if (strcmp(name, "set_brightness") == 0) {
    const int level = doc["args"]["level"].is<int>()
                          ? constrain(doc["args"]["level"].as<int>(), 0, 255)
                          : DEFAULT_BRIGHTNESS;
    if (_callbacks.onBrightness) {
      _callbacks.onBrightness(level);
    }
    result = String("Brightness set to ") + level;
  } else if (strcmp(name, "set_volume") == 0) {
    const int level = doc["args"]["level"].is<int>()
                          ? constrain(doc["args"]["level"].as<int>(), 0, 255)
                          : DEFAULT_VOLUME;
    if (_callbacks.onVolume) {
      _callbacks.onVolume(level);
    }
    result = String("Volume set to ") + level;
  } else if (strcmp(name, "set_speaker") == 0) {
    const char *mode = doc["args"]["mode"];
    if (!mode) {
      result = "Missing mode (use 'internal' or 'external')";
    } else if (!_callbacks.onSetSpeaker) {
      result = "Speaker control unavailable";
    } else if (_callbacks.onSetSpeaker(mode)) {
      result = String("Speaker set to ") + mode;
    } else {
      result = "Invalid mode (use 'internal' or 'external')";
    }
  } else if (strcmp(name, "set_external_speaker_gain") == 0) {
    if (!doc["args"]["gain"].is<int>()) {
      result = "Missing gain (integer 1..64)";
    } else if (!_callbacks.onSetExternalGain) {
      result = "Gain control unavailable";
    } else {
      const int gain = doc["args"]["gain"].as<int>();
      if (_callbacks.onSetExternalGain(gain)) {
        result = String("External speaker gain set to ") + constrain(gain, 1, 64);
      } else {
        result = "Gain out of range (use 1..64)";
      }
    }
  } else if (strcmp(name, "get_device_status") == 0) {
    result = _callbacks.getDeviceStatusJson ? _callbacks.getDeviceStatusJson()
                                            : "{}";
  } else if (strcmp(name, "show_text") == 0) {
    const char *text = doc["args"]["text"];
    if (text && _callbacks.onShowText) {
      _callbacks.onShowText(text);
    }
    result = "Text displayed";
  } else if (strcmp(name, "play_sound") == 0) {
    const char *sound = doc["args"]["sound"];
    const bool ok =
        sound && _callbacks.onPlaySound && _callbacks.onPlaySound(sound);
    result = ok ? String("Played sound: ") + sound : "Unknown sound";
  } else if (strcmp(name, "play_melody") == 0) {
    const char *melody = doc["args"]["notes"];
    const bool ok =
        melody && _callbacks.onPlayMelody && _callbacks.onPlayMelody(melody);
    result = ok ? "Melody played" : "Invalid melody";
  } else if (strcmp(name, "power_off") == 0) {
    if (!_callbacks.onPowerOff) {
      result = "Power off unavailable";
      sendToolResponse(name, id, result);
      return;
    }

    sendToolResponse(name, id, "Powering off");
    delay(100);
    _callbacks.onPowerOff();
    return;
  }

  sendToolResponse(name, id, result);
}

void LiveSessionService::sendToolResponse(const char *name, const char *id,
                                          const String &result) {
  JsonDocument response;
  response["type"] = "tool_response";
  response["name"] = name;
  response["id"] = id;
  response["result"] = result;

  String encoded;
  serializeJson(response, encoded);
  _ws.send(encoded.c_str());
  Serial.printf("[Tool] %s -> %s\n", name, result.c_str());
}

String LiveSessionService::endpointBaseUrl(const ServerEndpoint &endpoint) const {
  const char *scheme = httpSchemeForEndpoint(endpoint);
  String url = String(scheme) + "://" + endpointHostForConnection(endpoint);
  if (endpoint.port != 80 && endpoint.port != 443) {
    url += ":" + String(endpoint.port);
  }
  return url;
}
