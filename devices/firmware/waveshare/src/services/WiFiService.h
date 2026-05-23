#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>

/**
 * @brief Manages WiFi connectivity, saved credentials, and captive-portal
 * setup.
 */
class WiFiService {
public:
  /// Initialize preferences-backed WiFi state.
  void init();

  /// Service captive-portal DNS and HTTP handlers.
  void poll();

  /// Try connecting to the saved WiFi networks in priority order.
  bool connectKnownNetworks();

  /// Start the temporary captive portal used for provisioning WiFi.
  bool startCaptivePortal();

  /// Stop captive-portal DNS and HTTP services.
  void stopCaptivePortal();

  /// Whether the captive portal is currently serving requests.
  bool isCaptivePortalActive() const { return _captivePortalActive; }

  /**
   * @brief Consume a pending provisioning-success notification.
   * @param ssid Receives the SSID that was just provisioned.
   * @return True when a new provisioning success was pending.
   */
  bool consumeProvisioningSuccess(String &ssid);

  /// Disconnect from the current WiFi network.
  void disconnect();

  /// Clear runtime state and stop any active portal/network session.
  void reset();

  /// Whether the device currently has an active WiFi connection.
  bool isConnected() const;

  /// SSID of the currently connected network, if any.
  String ssid() const;

  /// Local IP address as a string.
  String localIp() const;

  /// SSID broadcast by the provisioning captive portal.
  String captivePortalSsid() const { return kCaptivePortalSsid; }

  /// Gateway address used by the captive portal.
  String captivePortalIp() const;

private:
  /**
   * @brief Stored WiFi credential entry.
   */
  struct SavedNetwork {
    /// Network SSID.
    String ssid;

    /// Network password.
    String password;

    /// Human-readable label shown in UI/debug output.
    String label;
  };

  /// Maximum number of networks persisted in preferences.
  static constexpr int kMaxSavedNetworks = 5;

  /// Preferences namespace for WiFi credentials.
  static constexpr const char *kPrefsNamespace = "wifi";

  /// SSID used while the captive portal is active.
  static constexpr const char *kCaptivePortalSsid = "chat-stick-setup";

  /// DNS port used to hijack captive-portal requests.
  static constexpr byte kDnsPort = 53;

  /// Maximum number of scan results retained for the portal page.
  static constexpr int kMaxScanResults = 8;

  /// Preferences handle for saved network storage.
  Preferences _prefs;

  /// DNS server used for captive-portal redirection.
  DNSServer _dnsServer;

  /// HTTP server used for captive-portal pages.
  WebServer _portalServer{80};

  /// Whether preferences have been initialized.
  bool _prefsReady = false;

  /// Whether captive-portal services are currently running.
  bool _captivePortalActive = false;

  /// Whether portal routes have already been registered.
  bool _portalRoutesConfigured = false;

  /// Whether a new network was provisioned since last consumption.
  bool _portalProvisioned = false;

  /// SSID most recently provisioned through the portal.
  String _provisionedSsid;

  /// In-memory cache of saved WiFi credentials.
  SavedNetwork _savedNetworks[kMaxSavedNetworks];

  /// Number of valid entries in _savedNetworks.
  int _savedNetworkCount = 0;

  /// Cached SSID scan results shown in the portal.
  String _scanResults[kMaxScanResults];

  /// Number of valid entries in _scanResults.
  int _scanResultCount = 0;

  /// Load saved networks from preferences into memory.
  void loadSavedNetworks();

  /// Persist the in-memory saved-network list.
  void writeSavedNetworks();

  /**
   * @brief Save or update a WiFi credential entry.
   * @param ssid Network SSID.
   * @param password Network password.
   * @param label Human-readable label.
   */
  void rememberNetwork(const String &ssid, const String &password,
                       const String &label);

  /**
   * @brief Connect to a specific WiFi network.
   * @param ssid Network SSID.
   * @param password Network password.
   * @param label Human-readable label for logs/UI.
   * @return True on successful association.
   */
  bool connectToNetwork(const String &ssid, const String &password,
                        const String &label);

  /// Register HTTP routes for the captive portal.
  void configureCaptivePortalRoutes();

  /// Refresh the cached list of scanned nearby networks.
  void refreshScanResults();

  /**
   * @brief Build the captive-portal HTML page.
   * @param message Optional status message to embed in the page.
   * @return Complete HTML response.
   */
  String portalHtml(const String &message = "") const;

  /// Handle the captive portal root page request.
  void handlePortalRoot();

  /// Handle the captive portal credentials submission.
  void handlePortalSave();

  /// Redirect an arbitrary request back to the portal root.
  void redirectToPortal();
};
