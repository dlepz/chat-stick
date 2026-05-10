#include "LiveSessionService.h"

#include "../credentials.h"
#include "../diag/Log.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

using namespace websockets;

namespace {
// Decode a standard base64 string into `out`. Returns the decoded byte count,
// or -1 if the input is malformed. Tolerates whitespace in the input.
int decodeBase64(const char *in, size_t inLen, uint8_t *out, size_t outCap) {
  static const int8_t T[256] = {
      // build table at runtime once below
      -1};
  static bool tableReady = false;
  static int8_t table[256];
  if (!tableReady) {
    for (int i = 0; i < 256; i++) table[i] = -1;
    const char *abc =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) table[(uint8_t)abc[i]] = i;
    tableReady = true;
  }
  (void)T;

  uint32_t buf = 0;
  int bits = 0;
  size_t outLen = 0;
  for (size_t i = 0; i < inLen; i++) {
    const char c = in[i];
    if (c == '=' || c == '\0') break;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    const int8_t v = table[(uint8_t)c];
    if (v < 0) return -1;
    buf = (buf << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outLen >= outCap) return -1;
      out[outLen++] = (uint8_t)((buf >> bits) & 0xFF);
    }
  }
  return (int)outLen;
}

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
    Log::client("OTA", "download failed status=%d", statusCode);
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    outError = "Missing firmware size";
    Log::client("OTA", "missing content length=%d", contentLength);
    return false;
  }

  Log::client("OTA", "downloading firmware bytes=%d", contentLength);
  if (!Update.begin(contentLength, U_FLASH)) {
    outError = "Update begin failed: " + updateError();
    Log::client("OTA", "Update.begin failed: %s", outError.c_str());
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
    Log::client("OTA", "Update.writeStream failed wrote=%u expected=%d err=%s",
                static_cast<unsigned>(written), contentLength,
                updateError().c_str());
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    outError = "Update end failed: " + updateError();
    Log::client("OTA", "Update.end failed: %s", outError.c_str());
    return false;
  }

  if (!Update.isFinished()) {
    outError = "Update incomplete";
    Log::client("OTA", "update incomplete");
    return false;
  }

  Log::client("OTA", "firmware update installed");
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

  Log::client("WS", "connecting %s://%s:%d%s", scheme, host.c_str(),
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
    Log::client("WS", "connect failed %s://%s:%d; will retry", scheme,
                host.c_str(), endpoint.port);
  }
}

void LiveSessionService::disconnect() {
  if (_ws.available()) {
    _ws.close();
  }
  _connected = false;
}

void LiveSessionService::setPreferredEndpointIndex(int endpointIndex) {
  if (endpointIndex < 0 || endpointIndex >= SERVER_ENDPOINT_COUNT) {
    return;
  }
  _nextServerIndex = endpointIndex;
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

  return getFromConfiguredEndpoints(
      "restoring session from",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/session/" + _chatId +
               "?device_id=" + DEVICE_ID;
      },
      [&outMessage](const HttpGetResponse &response) {
        if (response.statusCode == 404) {
          Log::client("HTTP", "session not found at endpoint %d",
                      response.endpointIndex);
          return HttpGetDecision::Continue;
        }

        if (response.statusCode != 200 || response.body.isEmpty()) {
          Log::client("HTTP", "session restore failed status=%d",
                      response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          Log::client("HTTP", "session restore returned invalid JSON");
          return HttpGetDecision::Continue;
        }

        const char *lastMessage = doc["last_message"];
        if (!lastMessage || !lastMessage[0]) {
          return HttpGetDecision::Stop;
        }

        outMessage = lastMessage;
        return HttpGetDecision::Success;
      });
}

bool LiveSessionService::fetchConversationHistory(ConversationSummary outEntries[],
                                                  int maxEntries,
                                                  int &outCount) {
  outCount = 0;
  if (maxEntries <= 0) {
    return false;
  }

  return getFromConfiguredEndpoints(
      "fetching history from",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/history/" + DEVICE_ID +
               "?device_id=" + DEVICE_ID;
      },
      [outEntries, maxEntries, &outCount](const HttpGetResponse &response) {
        if (response.statusCode != 200 || response.body.isEmpty()) {
          Log::client("HTTP", "history fetch failed status=%d",
                      response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body) || !doc.is<JsonArray>()) {
          Log::client("HTTP", "history response invalid");
          return HttpGetDecision::Continue;
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
        return HttpGetDecision::Success;
      });
}

bool LiveSessionService::checkFirmwareUpdate(FirmwareUpdateInfo &outInfo) {
  outInfo = FirmwareUpdateInfo{};

  return getFromConfiguredEndpoints(
      "checking firmware at",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/firmware/check?version=" +
               String(FIRMWARE_VERSION);
      },
      [&outInfo](const HttpGetResponse &response) {
        if (response.statusCode == 404) {
          return HttpGetDecision::Continue;
        }

        if (response.statusCode != 200 || response.body.isEmpty()) {
          Log::client("HTTP", "firmware check failed status=%d",
                      response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          Log::client("HTTP", "firmware check invalid JSON");
          return HttpGetDecision::Continue;
        }

        outInfo.available = doc["available"] | false;
        outInfo.latestVersion = doc["latest_version"] | FIRMWARE_VERSION;
        outInfo.notes = doc["notes"] | "";
        outInfo.downloadUrl = doc["download_url"] | "";
        return HttpGetDecision::Success;
      });
}

bool LiveSessionService::downloadAndApplyFirmwareUpdate(
    const String &downloadUrl, String &outError) {
  outError = "";

  if (downloadUrl.isEmpty()) {
    outError = "No download URL";
    return false;
  }

  Log::client("OTA", "downloading update from %s", downloadUrl.c_str());
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
    Log::client("WS", "opened");
    _connected = true;
    rememberSuccessfulEndpoint(_activeServerIndex);
    if (_callbacks.onStatus) {
      _callbacks.onStatus("Waiting for AI...");
    }
    break;

  case WebsocketsEvent::ConnectionClosed:
    Log::client("WS", "closed: %s", data.c_str());
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
    if ((doc["reset"] | false) && _callbacks.onConversationReset) {
      _callbacks.onConversationReset();
    }
    return;
  }

  if (strcmp(type, "ready") == 0) {
    Log::server("Gemini", "session ready");
    if (_callbacks.onReady) {
      _callbacks.onReady();
    }
    return;
  }

  if (strcmp(type, "turn_complete") == 0) {
    Log::server("Gemini", "turn complete");
    if (_callbacks.onTurnComplete) {
      _callbacks.onTurnComplete();
    }
    return;
  }

  if (strcmp(type, "drop_audio") == 0) {
    Log::server("Gemini", "drop audio interrupted");
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
    if (!source || strcmp(source, "model") != 0) {
      Log::server("Transcript", "%s: %s", source ? source : "?",
                  text ? text : "");
    }
    if (_callbacks.onTranscript && source && text) {
      _callbacks.onTranscript(String(source), String(text));
    }
    return;
  }

  if (strcmp(type, "error") == 0) {
    const char *category = doc["category"];
    const char *message = doc["message"];
    Log::server("Error", "[%s] %s", category ? category : "server",
                message ? message : "unknown");
    if (_callbacks.onError) {
      _callbacks.onError(category ? category : "server",
                         message ? message : "Server error");
    }
    return;
  }

  if (strcmp(type, "ignore_audio") == 0) {
    const char *reason = doc["reason"];
    const int bytes = doc["bytes"] | -1;
    const int avgAbs = doc["avg_abs"] | -1;
    const int chunks = doc["chunks"] | -1;
    Log::server("Gemini", "ignored audio: %s bytes=%d avg_abs=%d chunks=%d",
                reason ? reason : "ignored", bytes, avgAbs, chunks);
    if (_callbacks.onIgnoredAudio) {
      _callbacks.onIgnoredAudio(reason ? reason : "ignored");
    }
    return;
  }

  if (strcmp(type, "show_image_pending") == 0) {
    if (_callbacks.onShowImagePending) {
      _callbacks.onShowImagePending();
    }
    return;
  }

  if (strcmp(type, "show_image_failed") == 0) {
    Log::server("Image", "generation failed");
    if (_callbacks.onShowImageFailed) {
      _callbacks.onShowImageFailed();
    }
    return;
  }

  if (strcmp(type, "show_image") == 0) {
    const char *data = doc["data"];
    const int width = doc["width"] | 0;
    const int height = doc["height"] | 0;
    if (!data || width <= 0 || height <= 0) {
      Log::server("Image", "show_image missing data or dimensions");
      return;
    }
    const size_t encodedLen = strlen(data);
    const size_t expectedBytes = (size_t)((width * height + 7) / 8);
    uint8_t *packed = (uint8_t *)malloc(expectedBytes);
    if (!packed) {
      Log::server("Image", "failed to allocate decode buffer");
      if (_callbacks.onShowImageFailed) _callbacks.onShowImageFailed();
      return;
    }
    const int decoded = decodeBase64(data, encodedLen, packed, expectedBytes);
    if (decoded < 0 || (size_t)decoded < expectedBytes) {
      Log::server("Image", "decode failed got=%d expected=%u", decoded,
                  (unsigned)expectedBytes);
      free(packed);
      if (_callbacks.onShowImageFailed) _callbacks.onShowImageFailed();
      return;
    }
    Log::server("Image", "show_image %dx%d bytes=%u", width, height,
                (unsigned)expectedBytes);
    if (_callbacks.onShowImage) {
      _callbacks.onShowImage(packed, expectedBytes, width, height);
    }
    free(packed);
    return;
  }

  if (strcmp(type, "voice_changed") == 0) {
    const char *voice = doc["voice"];
    if (voice && *voice) {
      _voice = voice;
      Log::server("Voice", "changed to %s", voice);
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
  Log::server("Tool", "%s -> %s", name, result.c_str());
}

String LiveSessionService::endpointBaseUrl(const ServerEndpoint &endpoint) const {
  const char *scheme = httpSchemeForEndpoint(endpoint);
  String url = String(scheme) + "://" + endpointHostForConnection(endpoint);
  if (endpoint.port != 80 && endpoint.port != 443) {
    url += ":" + String(endpoint.port);
  }
  return url;
}

bool LiveSessionService::performEndpointGet(const ServerEndpoint &endpoint,
                                            const String &url,
                                            int &statusCode,
                                            String &body) const {
  statusCode = -1;
  body = "";

  HTTPClient http;
  if (urlUsesHttps(url)) {
    WiFiClientSecure client;
    if (endpoint.ca_cert) {
      client.setCACert(endpoint.ca_cert);
    } else {
      client.setInsecure();
    }

    if (!http.begin(client, url)) {
      Log::client("HTTP", "begin failed for %s", url.c_str());
      return false;
    }

    statusCode = http.GET();
    if (statusCode > 0) {
      body = http.getString();
    }
    http.end();
    return true;
  }

  WiFiClient client;
  if (!http.begin(client, url)) {
    Log::client("HTTP", "begin failed for %s", url.c_str());
    return false;
  }

  statusCode = http.GET();
  if (statusCode > 0) {
    body = http.getString();
  }
  http.end();
  return true;
}

bool LiveSessionService::getFromConfiguredEndpoints(
    const char *logAction, const EndpointUrlBuilder &buildUrl,
    const HttpGetHandler &handleResponse) {
  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url = buildUrl(endpoint);

    Log::client("HTTP", "%s %s", logAction, url.c_str());

    HttpGetResponse response;
    response.endpointIndex = index;
    if (!performEndpointGet(endpoint, url, response.statusCode,
                            response.body)) {
      continue;
    }

    const HttpGetDecision decision = handleResponse(response);
    if (decision == HttpGetDecision::Success) {
      rememberSuccessfulEndpoint(index);
      return true;
    }
    if (decision == HttpGetDecision::Stop) {
      return false;
    }
  }

  return false;
}

void LiveSessionService::rememberSuccessfulEndpoint(int endpointIndex) {
  if (endpointIndex < 0 || endpointIndex >= SERVER_ENDPOINT_COUNT) {
    return;
  }

  _nextServerIndex = endpointIndex;
  if (_callbacks.onEndpointIndexChanged) {
    _callbacks.onEndpointIndexChanged(endpointIndex);
  }
}
