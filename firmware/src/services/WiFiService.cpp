#include "WiFiService.h"

#include "../Config.h"
#include "../credentials.h"
#include "../diag/Log.h"
#include <WiFi.h>
#include <time.h>

/**
 * @brief Open preferences storage and load any saved WiFi networks.
 */
void WiFiService::init() {
  _prefsReady = _prefs.begin(kPrefsNamespace, false);
  if (_prefsReady) {
    loadSavedNetworks();
  } else {
    Log::client("WiFi", "preferences init failed; saved networks disabled");
  }
}

/**
 * @brief Service captive-portal DNS and HTTP handlers when active.
 */
void WiFiService::poll() {
  if (!_captivePortalActive) {
    return;
  }

  _dnsServer.processNextRequest();
  _portalServer.handleClient();
}

/**
 * @brief Try connecting to saved and compiled-in networks in priority order.
 * @return True on a successful association.
 */
bool WiFiService::connectKnownNetworks() {
  stopCaptivePortal();
  WiFi.mode(WIFI_STA);

  for (int i = 0; i < _savedNetworkCount; i++) {
    const SavedNetwork &network = _savedNetworks[i];
    if (connectToNetwork(network.ssid, network.password, network.label)) {
      return true;
    }
  }

  for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
    if (connectToNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                         WIFI_NETWORKS[i].label)) {
      rememberNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                      WIFI_NETWORKS[i].label);
      return true;
    }
  }

  Log::client("WiFi", "all networks failed");
  return false;
}

/**
 * @brief Start the soft-AP captive portal used to provision WiFi credentials.
 * @return True when the portal AP, DNS, and HTTP server all start.
 */
bool WiFiService::startCaptivePortal() {
  stopCaptivePortal();
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);

  refreshScanResults();

  if (!WiFi.softAP(kCaptivePortalSsid)) {
    Log::client("WiFi", "failed to start captive portal AP");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  delay(100);
  _dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  configureCaptivePortalRoutes();
  _portalServer.begin();
  _captivePortalActive = true;
  _portalProvisioned = false;
  _provisionedSsid = "";

  Log::client("WiFi", "captive portal started ssid=%s ip=%s",
              kCaptivePortalSsid, WiFi.softAPIP().toString().c_str());
  return true;
}

/**
 * @brief Stop captive-portal DNS and HTTP services and disable the soft-AP.
 */
void WiFiService::stopCaptivePortal() {
  if (_captivePortalActive) {
    _dnsServer.stop();
    _portalServer.stop();
    _captivePortalActive = false;
  }

  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.softAPdisconnect(true);
  }
}

/**
 * @brief Consume a pending portal-provisioning notification and stop the portal.
 * @param ssid Receives the SSID that was just provisioned.
 * @return True when a new provisioning event was pending.
 */
bool WiFiService::consumeProvisioningSuccess(String &ssid) {
  if (!_portalProvisioned) {
    return false;
  }

  ssid = _provisionedSsid;
  _portalProvisioned = false;
  _provisionedSsid = "";
  stopCaptivePortal();
  return true;
}

/**
 * @brief Disconnect from any active WiFi network and stop the captive portal.
 */
void WiFiService::disconnect() {
  stopCaptivePortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

/**
 * @brief Clear saved networks and stop any active portal/network session.
 */
void WiFiService::reset() {
  _savedNetworkCount = 0;
  if (_prefsReady) {
    _prefs.clear();
  }
  disconnect();
}

/**
 * @brief Whether the station interface is currently associated.
 * @return True when WiFi reports a connected status.
 */
bool WiFiService::isConnected() const { return WiFi.status() == WL_CONNECTED; }

/**
 * @brief SSID of the currently connected network.
 * @return SSID string, or empty when disconnected.
 */
String WiFiService::ssid() const { return isConnected() ? WiFi.SSID() : ""; }

/**
 * @brief Current station IP address.
 * @return IP string, or empty when disconnected.
 */
String WiFiService::localIp() const {
  return isConnected() ? WiFi.localIP().toString() : "";
}

/**
 * @brief Captive portal soft-AP gateway IP.
 * @return IP string, or empty when the portal is not active.
 */
String WiFiService::captivePortalIp() const {
  return _captivePortalActive ? WiFi.softAPIP().toString() : "";
}

/**
 * @brief Load persisted WiFi credentials from preferences into memory.
 */
void WiFiService::loadSavedNetworks() {
  _savedNetworkCount = constrain(_prefs.getUChar("count", 0), 0,
                                 kMaxSavedNetworks);

  for (int i = 0; i < _savedNetworkCount; i++) {
    const String suffix = String(i);
    _savedNetworks[i].ssid = _prefs.getString(("ssid_" + suffix).c_str(), "");
    _savedNetworks[i].password =
        _prefs.getString(("pass_" + suffix).c_str(), "");
    _savedNetworks[i].label =
        _prefs.getString(("label_" + suffix).c_str(), "");

    if (_savedNetworks[i].label.isEmpty()) {
      _savedNetworks[i].label = "Saved";
    }
  }

  Log::client("WiFi", "loaded saved networks count=%d", _savedNetworkCount);
}

/**
 * @brief Persist the in-memory saved-network list back to preferences.
 */
void WiFiService::writeSavedNetworks() {
  if (!_prefsReady) {
    return;
  }

  _prefs.putUChar("count", static_cast<uint8_t>(_savedNetworkCount));
  for (int i = 0; i < kMaxSavedNetworks; i++) {
    const String suffix = String(i);
    const String ssidKey = "ssid_" + suffix;
    const String passKey = "pass_" + suffix;
    const String labelKey = "label_" + suffix;

    if (i < _savedNetworkCount) {
      _prefs.putString(ssidKey.c_str(), _savedNetworks[i].ssid);
      _prefs.putString(passKey.c_str(), _savedNetworks[i].password);
      _prefs.putString(labelKey.c_str(), _savedNetworks[i].label);
    } else {
      _prefs.remove(ssidKey.c_str());
      _prefs.remove(passKey.c_str());
      _prefs.remove(labelKey.c_str());
    }
  }
}

/**
 * @brief Save or refresh a credential entry, moving it to the front of the list.
 * @param ssid Network SSID.
 * @param password Network password.
 * @param label Human-readable label.
 */
void WiFiService::rememberNetwork(const String &ssid, const String &password,
                                  const String &label) {
  if (!_prefsReady || ssid.isEmpty()) {
    return;
  }

  int existingIndex = -1;
  for (int i = 0; i < _savedNetworkCount; i++) {
    if (_savedNetworks[i].ssid == ssid) {
      existingIndex = i;
      break;
    }
  }

  SavedNetwork network{ssid, password, label};
  if (network.label.isEmpty()) {
    network.label = "Saved";
  }

  if (existingIndex > 0) {
    for (int i = existingIndex; i > 0; i--) {
      _savedNetworks[i] = _savedNetworks[i - 1];
    }
    _savedNetworks[0] = network;
  } else if (existingIndex == 0) {
    _savedNetworks[0] = network;
  } else {
    const int limit = min(_savedNetworkCount, kMaxSavedNetworks - 1);
    for (int i = limit; i > 0; i--) {
      _savedNetworks[i] = _savedNetworks[i - 1];
    }
    _savedNetworks[0] = network;
    if (_savedNetworkCount < kMaxSavedNetworks) {
      _savedNetworkCount++;
    }
  }

  writeSavedNetworks();
  Log::client("WiFi", "saved network %s to NVS", ssid.c_str());
}

/**
 * @brief Attempt to connect to a specific network with a timeout.
 * @param ssid Network SSID.
 * @param password Network password.
 * @param label Human-readable label used in logs.
 * @return True on successful association.
 */
bool WiFiService::connectToNetwork(const String &ssid, const String &password,
                                   const String &label) {
  if (ssid.isEmpty()) {
    return false;
  }

  Log::client("WiFi", "trying %s (%s)", ssid.c_str(),
              label.isEmpty() ? "Saved" : label.c_str());

  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED &&
         attempts < WIFI_CONNECT_TIMEOUT_SEC * 4) {
    delay(250);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Log::client("WiFi", "connected to %s ip=%s", ssid.c_str(),
                WiFi.localIP().toString().c_str());
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    configTime(0, 0, NTP_SERVER);
    return true;
  }

  WiFi.disconnect();
  return false;
}

/**
 * @brief Register HTTP routes for the captive portal pages and OS probes.
 */
void WiFiService::configureCaptivePortalRoutes() {
  if (_portalRoutesConfigured) {
    return;
  }

  _portalServer.on("/", HTTP_GET, [this]() { handlePortalRoot(); });
  _portalServer.on("/save", HTTP_POST, [this]() { handlePortalSave(); });

  _portalServer.on("/generate_204", HTTP_ANY, [this]() { redirectToPortal(); });
  _portalServer.on("/hotspot-detect.html", HTTP_ANY,
                   [this]() { redirectToPortal(); });
  _portalServer.on("/connecttest.txt", HTTP_ANY,
                   [this]() { redirectToPortal(); });
  _portalServer.on("/ncsi.txt", HTTP_ANY, [this]() { redirectToPortal(); });
  _portalServer.on("/fwlink", HTTP_ANY, [this]() { redirectToPortal(); });
  _portalServer.onNotFound([this]() { redirectToPortal(); });
  _portalRoutesConfigured = true;
}

/**
 * @brief Refresh the cached list of nearby networks shown in the portal page.
 */
void WiFiService::refreshScanResults() {
  _scanResultCount = 0;
  const int found = WiFi.scanNetworks(false, true);
  if (found <= 0) {
    return;
  }

  for (int i = 0; i < found && _scanResultCount < kMaxScanResults; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    bool duplicate = false;
    for (int j = 0; j < _scanResultCount; j++) {
      if (_scanResults[j] == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    _scanResults[_scanResultCount++] = ssid;
  }
  WiFi.scanDelete();
}

/**
 * @brief Build the captive-portal HTML page.
 * @param message Optional status message to embed at the top of the page.
 * @return Complete HTML response body.
 */
String WiFiService::portalHtml(const String &message) const {
  String html;
  html.reserve(2048);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>chat-stick setup</title>";
  html += "<style>body{font-family:sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem;background:#111;color:#f5f5f5}";
  html += "form{display:grid;gap:.75rem}input,select,button{font:inherit;padding:.75rem;border-radius:.5rem;border:1px solid #444}";
  html += "button{background:#fff;color:#111;font-weight:700}small{color:#bbb}.msg{padding:.75rem;border-radius:.5rem;background:#1d3b24;color:#d7ffd7}</style></head><body>";
  html += "<h1>chat-stick WiFi setup</h1>";
  html += "<p>Join this device, then submit your WiFi credentials. The device will save them and reconnect automatically.</p>";
  if (!message.isEmpty()) {
    html += "<div class='msg'>";
    html += message;
    html += "</div>";
  }
  html += "<form method='post' action='/save'>";
  html += "<label>Network<select name='ssid'><option value=''>Choose a network</option>";
  for (int i = 0; i < _scanResultCount; i++) {
    html += "<option value='";
    html += _scanResults[i];
    html += "'>";
    html += _scanResults[i];
    html += "</option>";
  }
  html += "</select></label>";
  html += "<label>Or enter SSID<input name='manual_ssid' placeholder='Network name'></label>";
  html += "<label>Password<input name='password' type='password' placeholder='Password'></label>";
  html += "<button type='submit'>Save and reconnect</button>";
  html += "</form><p><small>Portal IP: ";
  html += WiFi.softAPIP().toString();
  html += "</small></p></body></html>";
  return html;
}

/**
 * @brief Serve the captive portal landing page.
 */
void WiFiService::handlePortalRoot() {
  _portalServer.send(200, "text/html", portalHtml());
}

/**
 * @brief Handle a credential submission from the captive portal form.
 */
void WiFiService::handlePortalSave() {
  String selectedSsid = _portalServer.arg("ssid");
  String manualSsid = _portalServer.arg("manual_ssid");
  String password = _portalServer.arg("password");
  selectedSsid.trim();
  manualSsid.trim();
  password.trim();
  const String ssid = manualSsid.isEmpty() ? selectedSsid : manualSsid;

  if (ssid.isEmpty()) {
    _portalServer.send(400, "text/html",
                       portalHtml("Enter an SSID or choose one from the list."));
    return;
  }

  rememberNetwork(ssid, password, "Portal");
  _provisionedSsid = ssid;
  _portalProvisioned = true;
  _portalServer.send(200, "text/html",
                     portalHtml("Saved. Return to the device; it is reconnecting now."));
  Log::client("WiFi", "captive portal saved credentials for %s", ssid.c_str());
}

/**
 * @brief Redirect any unrecognized request back to the captive portal root.
 */
void WiFiService::redirectToPortal() {
  _portalServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString(),
                           true);
  _portalServer.send(302, "text/plain", "");
}
