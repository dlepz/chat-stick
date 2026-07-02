#include "LiveSessionService.h"

#include "../credentials.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

using namespace websockets;

namespace {
constexpr uint16_t kHttpGetTimeoutMs = 3000;
}

void LiveSessionService::init(const LiveSessionCallbacks &callbacks) {
  _callbacks = callbacks;
  attachWebsocketHandlers();
}

void LiveSessionService::attachWebsocketHandlers() {
  _ws.onMessage([this](WebsocketsMessage msg) { handleMessage(msg); });
  _ws.onEvent(
      [this](WebsocketsEvent event, String data) { handleEvent(event, data); });
}

void LiveSessionService::connect() {
  if (_ws.available()) {
    _ws.close();
  }
  _ws = WebsocketsClient();
  attachWebsocketHandlers();
  _connected = false;

  _activeServerIndex = _nextServerIndex;
  const ServerEndpoint &endpoint = SERVER_ENDPOINTS[_nextServerIndex];
  _nextServerIndex = (_nextServerIndex + 1) % SERVER_ENDPOINT_COUNT;

  const String path = String(SERVER_PATH) + "?device_id=" + DEVICE_ID;
  const String chatQuery = _chatId.isEmpty() ? "" : "&chat_id=" + _chatId;
  const char *scheme = endpoint.port == 443 ? "wss" : "ws";

  if (_callbacks.onStatus) {
    _callbacks.onStatus("Connecting...");
  }

  Serial.printf("[WS] Connecting to %s://%s:%d%s\n", scheme, endpoint.host,
                endpoint.port, (path + chatQuery).c_str());

  if (endpoint.port == 443) {
    if (endpoint.ca_cert) {
      _ws.setCACert(endpoint.ca_cert);
    } else {
      _ws.setInsecure();
    }
    _connected =
        _ws.connectSecure(endpoint.host, endpoint.port, (path + chatQuery).c_str());
  } else {
    // Important: do not call setInsecure() for plain ws:// endpoints.
    // ArduinoWebsockets may otherwise leave the client in a TLS mode and fail
    // local dev connections with SSL EOF errors.
    _connected = _ws.connect(endpoint.host, endpoint.port, (path + chatQuery).c_str());
  }

  if (!_connected) {
    Serial.printf("[WS] Connect failed: %s://%s:%d — will retry\n", scheme,
                  endpoint.host, endpoint.port);
  } else {
    Serial.printf("[WS] Connected: %s://%s:%d\n", scheme, endpoint.host,
                  endpoint.port);
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

bool LiveSessionService::sendCancelTurn(const String &reason) {
  JsonDocument doc;
  doc["type"] = "cancel_turn";
  doc["reason"] = reason;

  String payload;
  serializeJson(doc, payload);
  return _ws.send(payload);
}

bool LiveSessionService::sendText(const String &content) {
  JsonDocument doc;
  doc["type"] = "text";
  doc["content"] = content;

  String payload;
  serializeJson(doc, payload);
  return _ws.send(payload);
}

bool LiveSessionService::sendAudio(const int16_t *data, size_t len) {
  return _ws.sendBinary(reinterpret_cast<const char *>(data), len);
}

bool LiveSessionService::fetchLastAssistantMessage(String &outMessage) {
  outMessage = "";
  if (_chatId.isEmpty()) {
    return false;
  }

  return getFromConfiguredEndpoints(
      "Restoring session from",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/session/" + _chatId +
               "?device_id=" + DEVICE_ID;
      },
      [&outMessage](const HttpGetResponse &response) {
        if (response.statusCode == 404) {
          Serial.printf("[HTTP] Session not found at endpoint %d\n",
                        response.endpointIndex);
          return HttpGetDecision::Continue;
        }

        if (response.statusCode != 200 || response.body.isEmpty()) {
          Serial.printf("[HTTP] Session restore failed: status=%d\n",
                        response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          Serial.println("[HTTP] Session restore returned invalid JSON");
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
      "Fetching history from",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/history/" + DEVICE_ID +
               "?device_id=" + DEVICE_ID;
      },
      [outEntries, maxEntries, &outCount](const HttpGetResponse &response) {
        if (response.statusCode != 200 || response.body.isEmpty()) {
          Serial.printf("[HTTP] History fetch failed: status=%d\n",
                        response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body) || !doc.is<JsonArray>()) {
          Serial.println("[HTTP] History response invalid");
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

bool LiveSessionService::fetchLearningResources(
    const String &source, const String &query,
    LearningResourceSummary outEntries[], int maxEntries, int &outCount) {
  outCount = 0;
  if (maxEntries <= 0) {
    return false;
  }

  String encodedQuery = query;
  encodedQuery.replace(" ", "%20");
  encodedQuery.replace("&", "%26");

  return getFromConfiguredEndpoints(
      "Fetching learning resources from",
      [this, source, encodedQuery, maxEntries](const ServerEndpoint &endpoint) {
        String url = endpointBaseUrl(endpoint) +
                     "/device/learning-resources?device_id=" + DEVICE_ID +
                     "&limit=" + String(maxEntries) + "&q=" + encodedQuery;
        if (!source.isEmpty()) {
          url += "&source=" + source;
        }
        return url;
      },
      [outEntries, maxEntries, &outCount](const HttpGetResponse &response) {
        if (response.statusCode != 200 || response.body.isEmpty()) {
          Serial.printf("[HTTP] Learning resources fetch failed: status=%d\n",
                        response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          Serial.println("[HTTP] Learning resources response invalid JSON");
          return HttpGetDecision::Continue;
        }

        JsonArray rows = doc["results"].as<JsonArray>();
        for (JsonVariant row : rows) {
          if (outCount >= maxEntries) {
            break;
          }
          outEntries[outCount].resourceId = row["resource_id"] | "";
          outEntries[outCount].title = row["title"] | "Untitled";
          outEntries[outCount].subtitle = row["subtitle"] | "";
          outEntries[outCount].source = row["source"] | "";
          outEntries[outCount].level = row["level"] | "";
          if (!outEntries[outCount].resourceId.isEmpty()) {
            outCount++;
          }
        }
        return HttpGetDecision::Success;
      });
}

bool LiveSessionService::fetchInboxFlashcards(const String &mode,
                                              InboxFlashcardSummary outEntries[],
                                              int maxEntries, int &outCount,
                                              int &outDue, int &outTotal) {
  outCount = 0;
  outDue = 0;
  outTotal = 0;
  if (maxEntries <= 0) {
    return false;
  }

  const String safeMode = (mode == "all") ? "all" : "due";

  return getFromConfiguredEndpoints(
      "Fetching inbox flashcards from",
      [this, safeMode, maxEntries](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) +
               "/device/flashcards/inbox?device_id=" + DEVICE_ID +
               "&mode=" + safeMode + "&limit=" + String(maxEntries);
      },
      [outEntries, maxEntries, &outCount, &outDue, &outTotal](
          const HttpGetResponse &response) {
        if (response.statusCode != 200 || response.body.isEmpty()) {
          Serial.printf("[HTTP] Inbox fetch failed: status=%d\n",
                        response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          Serial.println("[HTTP] Inbox response invalid JSON");
          return HttpGetDecision::Continue;
        }

        outDue = doc["counts"]["due"] | 0;
        outTotal = doc["counts"]["total"] | 0;

        JsonArray rows = doc["cards"].as<JsonArray>();
        for (JsonVariant row : rows) {
          if (outCount >= maxEntries) {
            break;
          }
          const char *id = row["id"];
          const char *front = row["front"];
          const char *back = row["back"];
          if (!id || !front || !back) {
            continue;
          }
          outEntries[outCount].id = id;
          outEntries[outCount].front = front;
          outEntries[outCount].back = back;
          outCount++;
        }
        return HttpGetDecision::Success;
      });
}

bool LiveSessionService::gradeInboxFlashcard(const String &cardId,
                                             const String &grade) {
  if (cardId.isEmpty()) {
    return false;
  }
  const String safeGrade = (grade == "good") ? "good" : "again";

  JsonDocument req;
  req["device_id"] = DEVICE_ID;
  req["card_id"] = cardId;
  req["grade"] = safeGrade;
  String payload;
  serializeJson(req, payload);

  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url =
        endpointBaseUrl(endpoint) + "/device/flashcards/grade";

    Serial.printf("[HTTP] Grading flashcard %s=%s via %s\n", cardId.c_str(),
                  safeGrade.c_str(), url.c_str());

    int statusCode = -1;
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
        http.addHeader("Content-Type", "application/json");
        statusCode = http.POST(payload);
        http.end();
      } else {
        WiFiClient client;
        if (!http.begin(client, url)) {
          continue;
        }
        http.addHeader("Content-Type", "application/json");
        statusCode = http.POST(payload);
        http.end();
      }
    }

    if (statusCode == 200) {
      return true;
    }
    Serial.printf("[HTTP] Grade flashcard failed: status=%d\n", statusCode);
  }
  return false;
}

bool LiveSessionService::checkFirmwareUpdate(FirmwareUpdateInfo &outInfo) {
  outInfo = FirmwareUpdateInfo{};

  return getFromConfiguredEndpoints(
      "Checking firmware at",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/firmware/check?version=" +
               String(FIRMWARE_VERSION);
      },
      [&outInfo](const HttpGetResponse &response) {
        if (response.statusCode == 404) {
          return HttpGetDecision::Continue;
        }

        if (response.statusCode != 200 || response.body.isEmpty()) {
          Serial.printf("[HTTP] Firmware check failed: status=%d\n",
                        response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          Serial.println("[HTTP] Firmware check invalid JSON");
          return HttpGetDecision::Continue;
        }

        outInfo.available = doc["available"] | false;
        outInfo.latestVersion = doc["latest_version"] | FIRMWARE_VERSION;
        outInfo.notes = doc["notes"] | "";
        outInfo.downloadUrl = doc["download_url"] | "";
        return HttpGetDecision::Success;
      });
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
    TurnCompleteInfo info;
    info.turnId = doc["turn_id"] | 0;
    info.hadAudio = doc["had_audio"] | false;
    info.hadModelText = doc["had_model_text"] | false;
    info.hadToolActivity = doc["had_tool_activity"] | false;
    info.synthetic = doc["synthetic"] | false;
    info.reason = String(doc["reason"] | "");

    Serial.printf("[Server] Turn complete id=%lu audio=%d text=%d tool=%d synthetic=%d reason=%s\n",
                  static_cast<unsigned long>(info.turnId), info.hadAudio,
                  info.hadModelText, info.hadToolActivity, info.synthetic,
                  info.reason.c_str());
    if (_callbacks.onTurnComplete) {
      _callbacks.onTurnComplete(info);
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

  if (strcmp(type, "turn_feedback") == 0) {
    const char *color = doc["color"];
    const char *correction = doc["correction"];
    const char *reason = doc["reason"];
    Serial.printf("[Feedback] %s fix=%s reason=%s\n", color ? color : "?",
                  correction ? correction : "", reason ? reason : "");
    if (_callbacks.onTurnFeedback && color) {
      _callbacks.onTurnFeedback(String(color), String(correction ? correction : ""),
                                String(reason ? reason : ""));
    }
    return;
  }

  if (strcmp(type, "face_emotion") == 0) {
    const char *emotion = doc["emotion"];
    Serial.printf("[Face] emotion=%s\n", emotion ? emotion : "?");
    if (_callbacks.onFaceEmotion && emotion) {
      _callbacks.onFaceEmotion(String(emotion));
    }
    return;
  }

  if (strcmp(type, "face_control") == 0) {
    const char *emotion = doc["emotion"];
    const float lookX = doc["look_x"].is<float>() ? doc["look_x"].as<float>() : 0.0f;
    const float lookY = doc["look_y"].is<float>() ? doc["look_y"].as<float>() : 0.0f;
    const float spacing = doc["eye_spacing"].is<float>() ? doc["eye_spacing"].as<float>() : 52.0f;
    const float speed = doc["anim_speed"].is<float>() ? doc["anim_speed"].as<float>() : 1.0f;
    Serial.printf("[Face] control emotion=%s look=(%.2f,%.2f) spacing=%.1f speed=%.2f\n",
                  emotion ? emotion : "?", lookX, lookY, spacing, speed);
    if (_callbacks.onFaceControl && emotion) {
      _callbacks.onFaceControl(String(emotion), lookX, lookY, spacing, speed);
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
    Serial.printf("[Server] Gemini ignored audio: %s\n",
                  reason ? reason : "ignored");
    if (_callbacks.onIgnoredAudio) {
      _callbacks.onIgnoredAudio(reason ? reason : "ignored");
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
  Serial.printf("[Tool] %s -> %s\n", name, result.c_str());
}

String LiveSessionService::endpointBaseUrl(const ServerEndpoint &endpoint) const {
  const char *scheme = endpoint.port == 443 ? "https" : "http";
  String url = String(scheme) + "://" + endpoint.host;
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
  http.setTimeout(kHttpGetTimeoutMs);
  if (endpoint.port == 443) {
    WiFiClientSecure client;
    if (endpoint.ca_cert) {
      client.setCACert(endpoint.ca_cert);
    } else {
      client.setInsecure();
    }
    if (!http.begin(client, url)) {
      Serial.printf("[HTTP] Begin failed for %s\n", url.c_str());
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
    Serial.printf("[HTTP] Begin failed for %s\n", url.c_str());
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

    Serial.printf("[HTTP] %s %s\n", logAction, url.c_str());

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
}
