#include "network.h"

#include <DNSServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config.h"

static Settings *cfg = nullptr;
static DNSServer dnsServer;
static bool apOn = false;
static bool staEnabled = false;
static uint32_t lastStaOk = 0, lastStaRetry = 0;

static void startAp() {
  const char *pass = cfg->apPass.length() >= 8 ? cfg->apPass.c_str() : SETUP_AP_PASSWORD;
  WiFi.softAP(cfg->apSsid.c_str(), pass);
  // Captive Portal: jede DNS-Anfrage zeigt auf die Bridge.
  dnsServer.start(53, "*", WiFi.softAPIP());
  apOn = true;
  Serial.printf("# Access Point \"%s\" aktiv, http://%s\n", cfg->apSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  apOn = false;
  Serial.println("# Access Point beendet");
}

// Vorversionen hatten die Zugangsdaten fest einkompiliert; der WLAN-Stack hat
// sie zusätzlich selbst gespeichert. Beim ersten Start übernehmen, damit die
// Bridge nach dem OTA-Update im gleichen WLAN bleibt.
static void adoptStoredCredentials() {
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || !conf.sta.ssid[0]) return;
  char ssid[33], pass[65];
  memcpy(ssid, conf.sta.ssid, 32);
  ssid[32] = '\0';
  memcpy(pass, conf.sta.password, 64);
  pass[64] = '\0';
  cfg->staSsid = ssid;
  cfg->staPass = pass;
  settingsSave(*cfg);
  Serial.printf("# WLAN-Zugangsdaten uebernommen: %s\n", ssid);
}

void netBegin(Settings &s) {
  cfg = &s;
  WiFi.setHostname(HOSTNAME);

  if (s.apMode) {
    WiFi.mode(WIFI_AP);
    startAp();
    return;
  }

  WiFi.mode(WIFI_STA);
  if (!s.configured()) adoptStoredCredentials();
  if (!s.configured()) {
    Serial.println("# Nicht eingerichtet, Setup ueber den Access Point");
    WiFi.mode(WIFI_AP);
    startAp();
    return;
  }

  // Ohne Modem-Sleep: sonst verpasst der ESP32 ARP-Anfragen und ist für den
  // Cerbo zeitweise nicht erreichbar.
  WiFi.setSleep(false);
  if (s.staticIp) {
    if (WiFi.config(s.ip, s.gateway, s.subnet, s.dns))
      Serial.printf("# Feste IP %s\n", s.ip.toString().c_str());
    else
      Serial.println("# Feste IP ungueltig, verwende DHCP");
  }
  WiFi.setAutoReconnect(true);
  WiFi.begin(s.staSsid.c_str(), s.staPass.c_str());
  staEnabled = true;
  lastStaOk = lastStaRetry = millis();
}

void netLoop() {
  if (apOn) dnsServer.processNextRequest();
  if (!staEnabled) return;

  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    lastStaOk = now;
    // Notfall-AP schließen, sobald niemand mehr damit verbunden ist.
    if (apOn && WiFi.softAPgetStationNum() == 0) stopAp();
    return;
  }

  if (!apOn && now - lastStaOk >= FALLBACK_AP_AFTER_MS) {
    Serial.println("# WLAN nicht erreichbar, starte Access Point");
    startAp();
  }
  // AutoReconnect gibt z.B. bei falschem Passwort auf; selbst weiter versuchen,
  // aber nicht, solange jemand am AP hängt (der Suchlauf stört den AP-Kanal).
  if (now - lastStaRetry >= STA_RETRY_MS && (!apOn || WiFi.softAPgetStationNum() == 0)) {
    lastStaRetry = now;
    WiFi.disconnect();
    WiFi.begin(cfg->staSsid.c_str(), cfg->staPass.c_str());
  }
}

bool netApActive() { return apOn; }
