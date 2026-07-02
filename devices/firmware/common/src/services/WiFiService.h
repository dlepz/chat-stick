#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <functional>

/**
 * @brief Connects to known WiFi networks and hosts the provisioning portal.
 */
class WiFiService {
public:
  /// Callback signature for WiFi log lines.
  using LogCallback = std::function<void(const char *topic,
                                         const char *message)>;

  /**
   * @brief Register a callback for WiFi log lines.
   * @param callback Log sink callback.
   */
  void onLog(LogCallback callback) { _logCallback = callback; }

  /// Initialize Preferences storage and load saved networks.
  void init();

  /// Service captive-portal DNS and HTTP requests.
  void poll();

  /**
   * @brief Connect to a saved or configured network.
   * @return True when a network connection is established.
   */
  bool connectKnownNetworks();

  /**
   * @brief Start AP-mode captive portal provisioning.
   * @return True when the portal is running.
   */
  bool startCaptivePortal();

  /// Stop captive portal services and disable the soft AP.
  void stopCaptivePortal();

  /// Whether captive portal provisioning is active.
  bool isCaptivePortalActive() const { return _captivePortalActive; }

  /**
   * @brief Consume a completed provisioning event.
   * @param ssid Receives the newly saved SSID.
   * @return True once for each successful portal save.
   */
  bool consumeProvisioningSuccess(String &ssid);

  /// Disconnect WiFi and stop provisioning services.
  void disconnect();

  /// Clear saved networks and disconnect WiFi.
  void reset();

  /// Whether the station interface is connected.
  bool isConnected() const;

  /// Connected SSID, or empty when disconnected.
  String ssid() const;

  /// Station IP address, or empty when disconnected.
  String localIp() const;

  /// SSID broadcast by the captive portal.
  String captivePortalSsid() const { return kCaptivePortalSsid; }

  /// Captive portal AP IP address, or empty when inactive.
  String captivePortalIp() const;

private:
  /**
   * @brief Persisted WiFi credential and optional fast-connect hints.
   */
  struct SavedNetwork {
    /// Network SSID.
    String ssid;

    /// Network password.
    String password;

    /// User-visible source label.
    String label;

    /// Last known channel for fast reconnects.
    int32_t channel = 0;

    /// Last known BSSID for fast reconnects.
    uint8_t bssid[6] = {0};

    /// Whether bssid contains a valid fast-connect hint.
    bool hasBssid = false;
  };

  /// Maximum number of saved portal networks.
  static constexpr int kMaxSavedNetworks = 5;

  /// Preferences namespace for saved WiFi networks.
  static constexpr const char *kPrefsNamespace = "wifi";

  /// SSID broadcast by the captive portal.
  static constexpr const char *kCaptivePortalSsid = "chat-stick-setup";

  /// Captive portal DNS port.
  static constexpr byte kDnsPort = 53;

  /// Maximum SSIDs retained from a scan.
  static constexpr int kMaxScanResults = 8;

  /// Byte length of a WiFi BSSID.
  static constexpr size_t kBssidLength = 6;

  /// Initial timeout for the most likely network.
  static constexpr unsigned long kPrimaryConnectTimeoutMs = 8000;

  /// Initial timeout when fast-connect hints are available.
  static constexpr unsigned long kPrimaryHintConnectTimeoutMs = 4500;

  /// Timeout for visible fallback networks.
  static constexpr unsigned long kFallbackConnectTimeoutMs = 6000;

  /// Short timeout for fallback networks not present in the latest scan.
  static constexpr unsigned long kUnavailableConnectTimeoutMs = 2000;

  /// Preferences namespace handle.
  Preferences _prefs;

  /// DNS server used for captive portal interception.
  DNSServer _dnsServer;

  /// HTTP server used by the captive portal.
  WebServer _portalServer{80};

  /// Optional WiFi log sink.
  LogCallback _logCallback;

  /// Whether Preferences storage opened successfully.
  bool _prefsReady = false;

  /// Whether the captive portal is currently running.
  bool _captivePortalActive = false;

  /// Whether portal HTTP routes have been installed.
  bool _portalRoutesConfigured = false;

  /// Whether the portal has received new credentials.
  bool _portalProvisioned = false;

  /// SSID saved by the most recent portal submission.
  String _provisionedSsid;

  /// Saved network records, most recently successful first.
  SavedNetwork _savedNetworks[kMaxSavedNetworks];

  /// Number of valid saved network records.
  int _savedNetworkCount = 0;

  /// Unique SSIDs observed in the most recent scan.
  String _scanResults[kMaxScanResults];

  /// Number of valid entries in _scanResults.
  int _scanResultCount = 0;

  /// Load saved networks from Preferences storage.
  void loadSavedNetworks();

  /// Write saved networks to Preferences storage.
  void writeSavedNetworks();

  /**
   * @brief Insert or promote a saved network credential.
   * @param ssid Network SSID.
   * @param password Network password.
   * @param label User-visible source label.
   */
  void rememberNetwork(const String &ssid, const String &password,
                       const String &label);

  /**
   * @brief Attempt a single network connection.
   * @param ssid Network SSID.
   * @param password Network password.
   * @param label User-visible source label for logs.
   * @param timeoutMs Connection timeout in milliseconds.
   * @param channel Optional fast-connect channel hint.
   * @param bssid Optional fast-connect BSSID hint.
   * @return True when connected.
   */
  bool connectToNetwork(const String &ssid, const String &password,
                        const String &label, unsigned long timeoutMs,
                        int32_t channel = 0, const uint8_t *bssid = nullptr);

  /**
   * @brief Check whether an SSID already exists in saved networks.
   * @param ssid SSID to search for.
   * @return True when saved.
   */
  bool hasSavedNetwork(const String &ssid) const;

  /**
   * @brief Check whether an SSID appeared in the latest scan results.
   * @param ssid SSID to search for.
   * @return True when seen in the latest scan.
   */
  bool isScannedSsid(const String &ssid) const;

  /// Install captive portal HTTP routes once.
  void configureCaptivePortalRoutes();

  /// Refresh the bounded list of scanned SSIDs.
  void refreshScanResults();

  /**
   * @brief Render the captive portal HTML page.
   * @param message Optional status message to show.
   * @return Complete HTML document.
   */
  String portalHtml(const String &message = "") const;

  /// Handle the captive portal root page.
  void handlePortalRoot();

  /// Handle captive portal credential submission.
  void handlePortalSave();

  /// Redirect OS connectivity-check requests to the portal root.
  void redirectToPortal();

  /// Emit a WiFi log line.
  void log(const char *fmt, ...) const;
};
