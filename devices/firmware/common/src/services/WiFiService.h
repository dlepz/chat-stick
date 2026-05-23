#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <functional>

class WiFiService {
public:
  using LogCallback = std::function<void(const char *topic,
                                         const char *message)>;

  void onLog(LogCallback callback) { _logCallback = callback; }

  void init();
  void poll();
  bool connectKnownNetworks();
  bool startCaptivePortal();
  void stopCaptivePortal();
  bool isCaptivePortalActive() const { return _captivePortalActive; }
  bool consumeProvisioningSuccess(String &ssid);
  void disconnect();
  void reset();

  bool isConnected() const;
  String ssid() const;
  String localIp() const;
  String captivePortalSsid() const { return kCaptivePortalSsid; }
  String captivePortalIp() const;

private:
  struct SavedNetwork {
    String ssid;
    String password;
    String label;
    int32_t channel = 0;
    uint8_t bssid[6] = {0};
    bool hasBssid = false;
  };

  static constexpr int kMaxSavedNetworks = 5;
  static constexpr const char *kPrefsNamespace = "wifi";
  static constexpr const char *kCaptivePortalSsid = "chat-stick-setup";
  static constexpr byte kDnsPort = 53;
  static constexpr int kMaxScanResults = 8;
  static constexpr size_t kBssidLength = 6;
  static constexpr unsigned long kPrimaryConnectTimeoutMs = 8000;
  static constexpr unsigned long kPrimaryHintConnectTimeoutMs = 4500;
  static constexpr unsigned long kFallbackConnectTimeoutMs = 6000;
  static constexpr unsigned long kUnavailableConnectTimeoutMs = 2000;

  Preferences _prefs;
  DNSServer _dnsServer;
  WebServer _portalServer{80};
  LogCallback _logCallback;
  bool _prefsReady = false;
  bool _captivePortalActive = false;
  bool _portalRoutesConfigured = false;
  bool _portalProvisioned = false;
  String _provisionedSsid;
  SavedNetwork _savedNetworks[kMaxSavedNetworks];
  int _savedNetworkCount = 0;
  String _scanResults[kMaxScanResults];
  int _scanResultCount = 0;

  void loadSavedNetworks();
  void writeSavedNetworks();
  void rememberNetwork(const String &ssid, const String &password,
                       const String &label);
  bool connectToNetwork(const String &ssid, const String &password,
                        const String &label, unsigned long timeoutMs,
                        int32_t channel = 0, const uint8_t *bssid = nullptr);
  bool hasSavedNetwork(const String &ssid) const;
  bool isScannedSsid(const String &ssid) const;
  void configureCaptivePortalRoutes();
  void refreshScanResults();
  String portalHtml(const String &message = "") const;
  void handlePortalRoot();
  void handlePortalSave();
  void redirectToPortal();
  void log(const char *fmt, ...) const;
};
